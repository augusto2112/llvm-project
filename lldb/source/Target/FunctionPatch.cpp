//===-- FunctionPatch.cpp -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/FunctionPatch.h"
#include "lldb/Breakpoint/BreakpointSite.h"
#include "lldb/Core/Address.h"
#include "lldb/Core/AddressRange.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/SourceManager.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Target/EntryTrampoline.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"
#include "lldb/Target/ThreadList.h"
#include "lldb/Utility/State.h"
#include "lldb/Utility/SupportFile.h"
#include "llvm/Support/Endian.h"
#include "llvm/TargetParser/Triple.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>

using namespace lldb_private;

namespace {
/// A build writes the binary after the sources it read, and a filesystem with
/// coarse timestamps can leave the two within a second of each other either
/// way. A refusal that fires on that noise would refuse every ordinary build.
constexpr std::chrono::seconds kMinSourceSkew{2};

/// Bytes each pool of site slots takes. One page holds enough slots that the
/// ordinary handful of sites costs a single allocation.
constexpr size_t kSlotPoolPageSize = 4096;

// The blocks written below are read by the generated source's own declarations
// of them, so their shape is a contract rather than this file's choice of
// layout.
static_assert(sizeof(PatchRingHeader) == kPatchRingHeaderSize,
              "the header written here is the header the inferior reads");
static_assert(sizeof(PatchSiteSlot) == kPatchSiteSlotSize,
              "the slot written here is the slot the inferior reads");
} // namespace

namespace lldb_private {

/// What one site does when its trap fires.
///
/// Kept beside the injections rather than in them: a \ref PatchInjection
/// describes the code to emit, and a callback is not part of that description.
struct PatchSiteCallback {
  BreakpointHitCallback OnTrap = nullptr;
  void *Baton = nullptr;
};

struct FunctionPatchManager::PatchedFunction {
  /// The entry the redirect is written over, which is also the key this is held
  /// under.
  lldb::addr_t Entry = LLDB_INVALID_ADDRESS;

  /// The function's own source, taken once. Every recompile starts here rather
  /// than from the last copy, so injections never stack.
  FunctionBodyText Body;

  /// The path the copy's `#line` directives name.
  std::string SourcePath;

  std::vector<PatchInjection> Injections;

  llvm::DenseMap<uint32_t, PatchSiteCallback> Callbacks;

  /// The instructions the redirect replaced, so the entry can be put back.
  std::array<uint8_t, kEntryTrampolineSize> OriginalBytes = {};

  /// Where the copy the redirect points at begins. Invalid until a compile has
  /// succeeded.
  lldb::addr_t CopyAddress = LLDB_INVALID_ADDRESS;

  lldb::ModuleSP CurrentModule;

  /// Copies a later recompile replaced. Held rather than dropped, because a
  /// thread already inside one of them returns through it.
  std::vector<lldb::ModuleSP> RetiredModules;
};

} // namespace lldb_private

llvm::StringRef lldb_private::ToString(PatchFailure Reason) {
  switch (Reason) {
  case PatchFailure::NotArm64:
    return "in-process evaluation is implemented for arm64 only";
  case PatchFailure::NoProcess:
    return "there is no running process to patch";
  case PatchFailure::NoSourceFile:
    return "the function's source file could not be read";
  case PatchFailure::SourceNewerThanBinary:
    return "the source on disk was written after the binary, so recompiling "
           "it would substitute a different function";
  case PatchFailure::BodyNotFound:
    return "the function's body could not be located in its source";
  case PatchFailure::StaticLocal:
    return "the body declares a static local, which a copy cannot share";
  case PatchFailure::EntryTooSmall:
    return "the function is too small to hold a redirect";
  case PatchFailure::ThreadInPatchRange:
    return "a thread is stopped inside the bytes the redirect would overwrite";
  case PatchFailure::BreakpointInPatchRange:
    return "a breakpoint sits inside the bytes the redirect would overwrite";
  case PatchFailure::CompileFailed:
    return "the recompiled function did not compile";
  case PatchFailure::CaptureNotScalar:
    return "the capture is not a scalar of eight bytes or fewer";
  case PatchFailure::Unsupported:
    return "the observation asks for something in-process evaluation does not "
           "implement";
  }
  return "unknown";
}

bool lldb_private::SourceSkewExceedsNoise(llvm::sys::TimePoint<> Source,
                                          llvm::sys::TimePoint<> Binary) {
  if (Source == llvm::sys::TimePoint<>() || Binary == llvm::sys::TimePoint<>())
    return false;
  return Source - Binary >= kMinSourceSkew;
}

namespace {

/// Builds the error a refusal returns.
///
/// Every refusal carries its reason's own prose, because a caller that has
/// fallen back to evaluating the expression at a stop has to be able to say
/// what it fell back from.
llvm::Error Refuse(PatchFailure Reason) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 ToString(Reason));
}

/// As above, with \p Detail naming which of the reason's several ways of
/// happening this was.
///
/// A reason names something a caller can act on, and failing to reach the
/// inferior at all is not one of them. Those are reported as there being no
/// process to patch -- a process that cannot be read or written is not one this
/// can patch -- and the detail carries what actually failed.
llvm::Error Refuse(PatchFailure Reason, const llvm::Twine &Detail) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 llvm::Twine(ToString(Reason)) + ": " + Detail);
}

/// Which refusal an extraction failure amounts to.
///
/// The extractor reports its reason as prose, so the reason is recovered by
/// comparing against that same prose rather than against a spelling of our own:
/// there is one spelling, so the two cannot drift.
PatchFailure ClassifyBodyFailure(llvm::StringRef Message) {
  if (Message == ToString(BodyExtractFailure::StaticLocal))
    return PatchFailure::StaticLocal;
  return PatchFailure::BodyNotFound;
}

/// The whole of \p File as one buffer.
///
/// Joined from the lines the manager hands back, each of which keeps its own
/// newline, so the result is the file byte for byte. Byte for byte is the
/// point: this text is recompiled, and anything reformatted here is a
/// difference between the copy and the original.
std::string ReadWholeFile(SourceManager::File &File) {
  std::string Text;
  const uint32_t Lines = File.GetNumLines();
  for (uint32_t Line = 1; Line <= Lines; ++Line) {
    std::string OneLine;
    if (!File.GetLine(Line, OneLine))
      break;
    Text += OneLine;
  }
  return Text;
}

/// Everything about a function that a patch is built from, read before anything
/// is written to the inferior.
struct FunctionFacts {
  FunctionBodyText Body;
  std::string SourcePath;

  /// Bytes in the range the entry falls in, which is the range a redirect
  /// overwrites. Zero when no range could be found, which refuses the patch.
  lldb::addr_t EntrySize = 0;
};

llvm::Expected<FunctionFacts> ReadFunctionFacts(Target &Tgt,
                                                lldb::addr_t Entry) {
  Address Resolved;
  if (!Tgt.ResolveLoadAddress(Entry, Resolved))
    return Refuse(PatchFailure::NoSourceFile,
                  "no loaded section covers the entry address");

  SymbolContext SC;
  Resolved.CalculateSymbolContext(&SC, lldb::eSymbolContextFunction |
                                           lldb::eSymbolContextModule);
  if (!SC.function || !SC.module_sp)
    return Refuse(PatchFailure::NoSourceFile,
                  "no debug info describes a function at the entry address");

  // The declaration, not the line table, because the copy is compiled from the
  // declaration onwards and so has to start where the declaration does.
  SupportFileNSP SourceFile = std::make_shared<SupportFile>();
  uint32_t DeclLine = 0;
  SC.function->GetStartLineSourceInfo(SourceFile, DeclLine);
  if (DeclLine == 0)
    return Refuse(PatchFailure::NoSourceFile,
                  "the function has no declaration line");

  SourceManager::FileSP Source = Tgt.GetSourceManager().GetFile(SourceFile);
  if (!Source || Source->GetNumLines() == 0)
    return Refuse(PatchFailure::NoSourceFile,
                  "\"" + SourceFile->GetSpecOnly().GetPath() +
                      "\" could not be read");

  FunctionFacts Facts;
  // The path the source manager resolved to, not the one debug info recorded: a
  // remapped tree is the copy that was actually read, and the copy's `#line`
  // has to name a file whoever reads the report can open.
  Facts.SourcePath = Source->GetSupportFile()->GetSpecOnly().GetPath();

  // Compared against the module the function landed in, which for a function in
  // a shared library is that library rather than the program. The mtimes are
  // taken the same way the reporting that describes this skew in prose takes
  // them, so a refusal and a report cannot disagree about the same two files.
  if (SourceSkewExceedsNoise(Source->GetTimestamp(),
                             FileSystem::Instance().GetModificationTime(
                                 SC.module_sp->GetFileSpec())))
    return Refuse(PatchFailure::SourceNewerThanBinary);

  llvm::Expected<FunctionBodyText> Body =
      ExtractFunctionBody(ReadWholeFile(*Source), DeclLine);
  if (!Body) {
    const std::string Message = llvm::toString(Body.takeError());
    return Refuse(ClassifyBodyFailure(Message), Message);
  }
  Facts.Body = std::move(*Body);

  // The range the entry falls in rather than the function's whole extent, since
  // a function whose code is in several pieces is redirected at the piece its
  // entry is in.
  AddressRange Range;
  if (SC.function->GetRangeContainingLoadAddress(Entry, Tgt, Range))
    Facts.EntrySize = Range.GetByteSize();
  return Facts;
}

/// Whether anything is depending on the bytes a redirect would overwrite.
llvm::Error CheckPatchRangeIsFree(Process &Proc, lldb::addr_t Entry) {
  const lldb::addr_t End = Entry + kEntryTrampolineSize;

  ThreadList &Threads = Proc.GetThreadList();
  const uint32_t ThreadCount = Threads.GetSize();
  for (uint32_t I = 0; I < ThreadCount; ++I) {
    lldb::ThreadSP T = Threads.GetThreadAtIndex(I);
    if (!T)
      continue;
    lldb::RegisterContextSP Regs = T->GetRegisterContext();
    if (!Regs)
      continue;
    const lldb::addr_t PC = Regs->GetPC();
    if (PC >= Entry && PC < End)
      return Refuse(PatchFailure::ThreadInPatchRange);
  }

  // A software breakpoint holds the byte it displaced and puts it back when it
  // is removed, so one inside the range would restore an old instruction over
  // part of the redirect long after the redirect was written.
  bool Overlaps = false;
  Proc.GetBreakpointSiteList().ForEach([&](BreakpointSite *Site) {
    const lldb::addr_t Start = Site->GetLoadAddress();
    // A site that reports no size still occupies the address it sits at.
    const lldb::addr_t Size = std::max<lldb::addr_t>(Site->GetByteSize(), 1);
    if (Start < End && Entry < Start + Size)
      Overlaps = true;
  });
  if (Overlaps)
    return Refuse(PatchFailure::BreakpointInPatchRange);

  return llvm::Error::success();
}

} // namespace

FunctionPatchManager::FunctionPatchManager(Target &Tgt) : m_target(Tgt) {}

FunctionPatchManager::~FunctionPatchManager() = default;

llvm::Expected<uint32_t>
FunctionPatchManager::Install(const PatchRequest &Request) {
  // The redirect is hand-encoded arm64 and nothing else has an encoding.
  if (m_target.GetArchitecture().GetTriple().getArch() != llvm::Triple::aarch64)
    return Refuse(PatchFailure::NotArm64);

  Process *Proc = m_target.GetProcessSP().get();
  // Stopped rather than merely alive: installing rewrites the inferior's code
  // and reads every thread's PC, and neither means anything while it runs.
  if (!Proc || !Proc->IsAlive() ||
      !StateIsStoppedState(Proc->GetState(), /*must_exist=*/true))
    return Refuse(PatchFailure::NoProcess);

  const lldb::addr_t Entry = Request.FunctionEntry;

  // A function seen for the first time is built into a local and only entered
  // into the map once its first site is installed, so a refusal anywhere below
  // leaves nothing behind claiming the function is patched.
  std::unique_ptr<PatchedFunction> Fresh;
  PatchedFunction *Fn = nullptr;
  if (auto It = m_functions.find(Entry); It != m_functions.end()) {
    Fn = It->second.get();
  } else {
    llvm::Expected<FunctionFacts> Facts = ReadFunctionFacts(m_target, Entry);
    if (!Facts)
      return Facts.takeError();

    if (Facts->EntrySize < kEntryTrampolineSize)
      return Refuse(PatchFailure::EntryTooSmall);

    Fresh = std::make_unique<PatchedFunction>();
    Fresh->Entry = Entry;
    Fresh->Body = std::move(Facts->Body);
    Fresh->SourcePath = std::move(Facts->SourcePath);

    // Read before anything is written, since these are the bytes that put the
    // function back. Read once, too: after the first install the entry holds
    // the redirect, so reading it again would save the redirect as the
    // original.
    Status ReadError;
    if (Proc->ReadMemory(Entry, Fresh->OriginalBytes.data(),
                         Fresh->OriginalBytes.size(),
                         ReadError) != Fresh->OriginalBytes.size())
      return Refuse(PatchFailure::NoProcess,
                    llvm::Twine("the function's entry could not be read: ") +
                        ReadError.AsCString());

    Fn = Fresh.get();
  }

  // Checked on every install rather than only the first, because every
  // recompile writes the redirect again over the same bytes.
  if (llvm::Error Err = CheckPatchRangeIsFree(*Proc, Entry))
    return std::move(Err);

  if (llvm::Error Err = EnsureRingBlock())
    return std::move(Err);

  llvm::Expected<lldb::addr_t> Slot = AllocateSiteSlot();
  if (!Slot)
    return Slot.takeError();

  const uint32_t SiteID = m_next_site_id++;

  // A gated site starts closed, because the debugger opens it when its own
  // state says the site should do anything. An ungated site never reads the
  // gate, so leaving it open costs nothing and describes the site honestly.
  if (!Request.Gated) {
    const uint8_t Open = 1;
    Status GateError;
    if (Proc->WriteMemory(*Slot + offsetof(PatchSiteSlot, Gate), &Open,
                          sizeof(Open), GateError) != sizeof(Open))
      return Refuse(PatchFailure::NoProcess,
                    llvm::Twine("the site's gate could not be written: ") +
                        GateError.AsCString());
  }

  PatchInjection Injection;
  Injection.SiteID = SiteID;
  Injection.Line = Request.Line;
  Injection.SlotAddress = *Slot;
  Injection.Condition = Request.Condition;
  Injection.Captures = Request.Captures;
  Injection.SkipFirst = Request.SkipFirst;
  Injection.OnlyHit = Request.OnlyHit;
  Injection.Gated = Request.Gated;
  Injection.WantStop = Request.WantStop;
  Fn->Injections.push_back(std::move(Injection));
  Fn->Callbacks[SiteID] = PatchSiteCallback{Request.OnTrap, Request.Baton};

  if (llvm::Error Err = Recompile(*Fn)) {
    // A compile that failed installed nothing, so the site it was for must not
    // be remembered as live in a function still running its previous copy.
    Fn->Injections.pop_back();
    Fn->Callbacks.erase(SiteID);
    return std::move(Err);
  }

  if (Fresh)
    m_functions[Entry] = std::move(Fresh);
  m_site_to_function[SiteID] = Entry;
  return SiteID;
}

llvm::Error FunctionPatchManager::Remove(uint32_t SiteID) {
  return llvm::make_error<llvm::StringError>("not yet implemented",
                                             llvm::inconvertibleErrorCode());
}

void FunctionPatchManager::SetGate(uint32_t SiteID, bool Open) {
  // not yet implemented
}

bool FunctionPatchManager::IsPatched(lldb::addr_t Entry) const {
  return m_functions.find(Entry) != m_functions.end();
}

llvm::Error FunctionPatchManager::EnsureRingBlock() {
  // Allocated once and never again. The address is compiled into every patch as
  // a literal, so a second block would leave code already running in the
  // program reporting into the first one.
  if (m_ring_address != LLDB_INVALID_ADDRESS)
    return llvm::Error::success();

  Process *Proc = m_target.GetProcessSP().get();
  if (!Proc)
    return Refuse(PatchFailure::NoProcess);

  // Readable and writable but not executable: the block is only ever the data a
  // patch reports through.
  Status AllocError;
  const lldb::addr_t Block = Proc->AllocateMemory(
      kPatchRingHeaderSize + kDefaultRingCapacity * kPatchRecordSize,
      lldb::ePermissionsReadable | lldb::ePermissionsWritable, AllocError);
  if (Block == LLDB_INVALID_ADDRESS)
    return Refuse(PatchFailure::NoProcess,
                  llvm::Twine("the record block could not be allocated: ") +
                      AllocError.AsCString());

  // Written a field at a time in the inferior's byte order rather than by
  // copying the struct, so the layout the inferior sees is stated here rather
  // than inherited from however the debugger's host happens to lay it out.
  uint8_t Header[kPatchRingHeaderSize] = {};
  llvm::support::endian::write64le(Header + offsetof(PatchRingHeader, Capacity),
                                   kDefaultRingCapacity);
  llvm::support::endian::write64le(Header +
                                       offsetof(PatchRingHeader, HighWater),
                                   kDefaultRingCapacity * 3 / 4);

  Status WriteError;
  if (Proc->WriteMemory(Block, Header, sizeof(Header), WriteError) !=
      sizeof(Header))
    return Refuse(PatchFailure::NoProcess,
                  llvm::Twine("the record block's header could not be "
                              "written: ") +
                      WriteError.AsCString());

  m_ring_address = Block;
  return llvm::Error::success();
}

llvm::Expected<lldb::addr_t> FunctionPatchManager::AllocateSiteSlot() {
  Process *Proc = m_target.GetProcessSP().get();
  if (!Proc)
    return Refuse(PatchFailure::NoProcess);

  // Sub-allocated from pages rather than allocated one at a time, because a
  // slot's own address is compiled into the patch that uses it and so has to
  // stay put for as long as that patch runs.
  constexpr size_t kSlotsPerPage = kSlotPoolPageSize / kPatchSiteSlotSize;
  if (m_slot_pool == LLDB_INVALID_ADDRESS ||
      m_slots_used_in_page >= kSlotsPerPage) {
    Status AllocError;
    const lldb::addr_t Page = Proc->AllocateMemory(
        kSlotPoolPageSize,
        lldb::ePermissionsReadable | lldb::ePermissionsWritable, AllocError);
    if (Page == LLDB_INVALID_ADDRESS)
      return Refuse(PatchFailure::NoProcess,
                    llvm::Twine("a page of site slots could not be "
                                "allocated: ") +
                        AllocError.AsCString());
    m_slot_pool = Page;
    m_slots_used_in_page = 0;
  }

  const lldb::addr_t Slot =
      m_slot_pool + m_slots_used_in_page * kPatchSiteSlotSize;
  ++m_slots_used_in_page;

  // A fresh site has no hits yet, and the page it came from holds whatever the
  // allocator left there.
  const uint8_t Zeroed[kPatchSiteSlotSize] = {};
  Status WriteError;
  if (Proc->WriteMemory(Slot, Zeroed, sizeof(Zeroed), WriteError) !=
      sizeof(Zeroed))
    return Refuse(PatchFailure::NoProcess,
                  llvm::Twine("a site's slot could not be cleared: ") +
                      WriteError.AsCString());

  return Slot;
}

llvm::Error FunctionPatchManager::Recompile(PatchedFunction &Fn) {
  // Compiling the copy and pointing the entry at it is not wired up yet.
  return llvm::Error::success();
}
