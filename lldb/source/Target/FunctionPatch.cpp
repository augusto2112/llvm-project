//===-- FunctionPatch.cpp -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/FunctionPatch.h"
#include "lldb/Breakpoint/Breakpoint.h"
#include "lldb/Breakpoint/BreakpointLocation.h"
#include "lldb/Breakpoint/BreakpointSite.h"
#include "lldb/Core/Address.h"
#include "lldb/Core/AddressRange.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleList.h"
#include "lldb/Core/SourceManager.h"
#include "lldb/Expression/DiagnosticManager.h"
#include "lldb/Expression/LLVMUserExpression.h"
#include "lldb/Expression/UserExpression.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Symbol/CompilerDeclContext.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Target/EntryTrampoline.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"
#include "lldb/Target/ThreadList.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/Utility/State.h"
#include "lldb/Utility/SupportFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Endian.h"
#include "llvm/TargetParser/Triple.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace lldb_private;

namespace {
/// A build writes the binary after the sources it read, and a filesystem with
/// coarse timestamps can leave the two within a second of each other either
/// way. A refusal that fires on that noise would refuse every ordinary build.
constexpr std::chrono::seconds kMinSourceSkew{2};

/// Bytes each pool of site slots takes. One page holds enough slots that the
/// ordinary handful of sites costs a single allocation.
constexpr size_t kSlotPoolPageSize = 4096;

/// Bytes one arm64 instruction occupies. Every one of them is this wide, which
/// is what lets the copy's traps be found by reading its bytes at this stride
/// rather than by driving a disassembler.
constexpr lldb::addr_t kInstructionSize = 4;

/// The arm64 encoding of `brk #0xf000`, which is what `__builtin_debugtrap()`
/// in the generated source compiles to. Read back out of the compiled copy
/// because nothing else records where the compiler put it.
constexpr uint32_t kDebugTrapOpcode = 0xD43E0000;

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
  lldb::BatonSP Baton;
};

struct FunctionPatchManager::PatchedFunction {
  /// The entry the redirect is written over, which is also the key this is held
  /// under.
  lldb::addr_t Entry = LLDB_INVALID_ADDRESS;

  /// The name the copy is found under in the module compiled from the copy's
  /// source. The copy keeps the original's name, so this is the original's.
  ConstString Name;

  /// The function's own source, taken once. Every recompile starts here rather
  /// than from the last copy, so injections never stack.
  FunctionBodyText Body;

  /// The path the copy's `#line` directives name.
  std::string SourcePath;

  std::vector<PatchInjection> Injections;

  llvm::DenseMap<uint32_t, PatchSiteCallback> Callbacks;

  /// The internal breakpoints that carry each site's callback, one per trap the
  /// copies compiled for that site contain.
  llvm::DenseMap<uint32_t, std::vector<lldb::break_id_t>> SiteBreakpoints;

  /// The instructions the redirect replaced, so the entry can be put back.
  std::array<uint8_t, kEntryTrampolineSize> OriginalBytes = {};

  /// Where the copy the redirect points at begins. Invalid until a compile has
  /// succeeded.
  lldb::addr_t CopyAddress = LLDB_INVALID_ADDRESS;

  lldb::ModuleSP CurrentModule;

  /// The expression the current copy's code belongs to. Held because dropping
  /// it takes \ref CurrentModule back out of the target's images, which is what
  /// describes the copy to the rest of lldb.
  lldb::UserExpressionSP CurrentExpression;

  /// Copies a later recompile replaced. Held rather than dropped, because a
  /// thread already inside one of them returns through it.
  std::vector<lldb::ModuleSP> RetiredModules;

  /// The expressions those copies belong to, held for the same reason and for
  /// as long.
  std::vector<lldb::UserExpressionSP> RetiredExpressions;
};

} // namespace lldb_private

llvm::StringRef lldb_private::ToString(PatchFailure Reason) {
  switch (Reason) {
  case PatchFailure::NotArm64:
    return "in-process evaluation is implemented for arm64 only";
  case PatchFailure::NoProcess:
    return "there is no running process to patch";
  case PatchFailure::InferiorAccessFailed:
    return "an operation on the inferior's memory failed";
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
    return "a breakpoint already occupies bytes the patch needs";
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
llvm::Error Refuse(PatchFailure Reason, const llvm::Twine &Detail) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 llvm::Twine(ToString(Reason)) + ": " + Detail);
}

/// Which refusal an extraction failure amounts to.
PatchFailure ClassifyBodyFailure(BodyExtractFailure Reason) {
  if (Reason == BodyExtractFailure::StaticLocal)
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

/// How many bytes of \p Fn's code the piece containing \p Addr occupies, or
/// zero if no piece of it does.
///
/// A function whose code is in several pieces is described by one range per
/// piece, and a redirect written at the entry can only rely on the piece the
/// entry is in. Asked of the function's block by address rather than by load
/// address, because the load-address form of the same question answers whether
/// a range was found without handing back the range it found.
lldb::addr_t CodeSizeContaining(Function &Fn, const Address &Addr) {
  AddressRange Range;
  if (!Fn.GetBlock(/*can_create=*/false).GetRangeContainingAddress(Addr, Range))
    return 0;
  return Range.GetByteSize();
}

/// Everything about a function that a patch is built from, read before anything
/// is written to the inferior.
struct FunctionFacts {
  FunctionBodyText Body;
  std::string SourcePath;

  /// The function's own name, which the copy compiled from its source will
  /// carry too, and is therefore how the copy is found.
  ConstString Name;

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
  Facts.Name = SC.function->GetName();
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
    BodyExtractFailure ExtractReason = BodyExtractFailure::NotFound;
    if (llvm::Error Leftover = llvm::handleErrors(
            Body.takeError(), [&](const BodyExtractError &Err) {
              ExtractReason = Err.reason();
            }))
      return std::move(Leftover);
    return Refuse(ClassifyBodyFailure(ExtractReason), ToString(ExtractReason));
  }
  Facts.Body = std::move(*Body);

  // The range the entry falls in rather than the function's whole extent, since
  // a function whose code is in several pieces is redirected at the piece its
  // entry is in.
  Facts.EntrySize = CodeSizeContaining(*SC.function, Resolved);
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
    return Refuse(PatchFailure::BreakpointInPatchRange,
                  "inside the bytes the redirect would overwrite");

  return llvm::Error::success();
}

/// One compiled copy of a patched function.
struct CompiledCopy {
  /// The expression the copy's code belongs to. Kept for as long as the copy is
  /// reachable, because letting go of it takes \ref Module back out of the
  /// target's images.
  lldb::UserExpressionSP Expression;

  /// What describes the copy to the rest of lldb: its line table is what makes
  /// a stop inside the copy report a line of the original file.
  lldb::ModuleSP Module;

  lldb::addr_t Address = LLDB_INVALID_ADDRESS;

  /// Bytes of code the copy occupies, which bounds the search for its traps.
  lldb::addr_t Size = 0;
};

/// Compiles \p Source into the inferior and finds the definition of \p Name it
/// contains.
///
/// Nothing is appended to the target and nothing is written over the original,
/// so a failure here leaves the program running exactly the code it was.
llvm::Expected<CompiledCopy> CompileCopy(Target &Tgt, llvm::StringRef Source,
                                         ConstString Name) {
  lldb::ProcessSP Proc = Tgt.GetProcessSP();
  if (!Proc)
    return Refuse(PatchFailure::NoProcess);

  EvaluateExpressionOptions Options;
  // The source is a definition rather than something to evaluate: nothing runs
  // now, and the code it compiles to outlives the call that compiled it.
  Options.SetExecutionPolicy(eExecutionPolicyTopLevel);
  // Not optional. The `#line` directives in the generated source only reach a
  // line table if one is emitted at all, and that line table is the whole
  // reason a stop inside the copy can name the original file and line.
  Options.SetGenerateDebugInfo(true);
  Options.SetLanguage(lldb::eLanguageTypeC99);
  Options.SetIgnoreBreakpoints(true);
  Options.SetTryAllThreads(false);
  Options.SetUnwindOnError(true);

  // The process rather than a frame: a definition is parsed in no frame's
  // scope, and offering one would only let whatever the program is stopped in
  // take part in resolving the names in the body.
  ExecutionContext ExeCtx;
  Proc->CalculateExecutionContext(ExeCtx);

  // Whatever this reports is not the verdict: asking for C is answered with a
  // note that C++ was used instead, alongside a perfectly usable expression.
  Status Unused;
  lldb::UserExpressionSP Expr(Tgt.GetUserExpressionForLanguage(
      Source, /*prefix=*/"", SourceLanguage(lldb::eLanguageTypeC99),
      Expression::eResultTypeAny, Options, /*ctx_obj=*/nullptr, Unused));
  if (!Expr)
    return Refuse(PatchFailure::CompileFailed,
                  llvm::Twine("no expression parser for the patched copy: ") +
                      Unused.AsCString());

  // Parsed rather than evaluated, and the parse is the whole of it: a top-level
  // expression's code is JIT'd into the inferior by the parse and never run, so
  // there is no result to wait for. Parsing here rather than through
  // Target::EvaluateExpression is what keeps the expression object, since the
  // module describing the copy lives exactly as long as it does.
  DiagnosticManager Diagnostics;
  if (!Expr->Parse(Diagnostics, ExeCtx, eExecutionPolicyTopLevel,
                   /*keep_result_in_memory=*/true,
                   /*generate_debug_info=*/true))
    return Refuse(PatchFailure::CompileFailed, Diagnostics.GetString());

  auto *JITExpr = llvm::dyn_cast<LLVMUserExpression>(Expr.get());
  if (!JITExpr)
    return Refuse(
        PatchFailure::CompileFailed,
        "the expression parser produced no JIT'd code to redirect to");

  CompiledCopy Copy;
  Copy.Expression = Expr;
  Copy.Module = JITExpr->TakeJITModule();
  if (!Copy.Module)
    return Refuse(PatchFailure::CompileFailed,
                  "nothing describes the compiled copy's code");

  // Looked up inside the one module rather than by evaluating `&name`: the copy
  // carries the original's name on purpose, so that a stop inside it names the
  // function the caller knows -- which is also what makes a lookup across the
  // whole target ambiguous the moment a copy exists.
  SymbolContextList Matches;
  ModuleFunctionSearchOptions SearchOptions;
  Copy.Module->FindFunctions(Name, CompilerDeclContext(),
                             lldb::eFunctionNameTypeFull |
                                 lldb::eFunctionNameTypeBase,
                             SearchOptions, Matches);
  if (Matches.GetSize() != 1)
    return Refuse(PatchFailure::CompileFailed,
                  llvm::Twine("the compiled copy holds ") +
                      llvm::Twine(Matches.GetSize()) + " definitions of \"" +
                      Name.GetStringRef() + "\" where it should hold one");

  Function *CopyFunction = Matches[0].function;
  if (!CopyFunction)
    return Refuse(PatchFailure::CompileFailed,
                  llvm::Twine("the compiled copy's \"") + Name.GetStringRef() +
                      "\" has no debug info describing it");

  Copy.Address = CopyFunction->GetAddress().GetLoadAddress(&Tgt);
  Copy.Size = CodeSizeContaining(*CopyFunction, CopyFunction->GetAddress());
  if (Copy.Address == LLDB_INVALID_ADDRESS || Copy.Size == 0)
    return Refuse(PatchFailure::CompileFailed,
                  "the compiled copy's code has no address in the process");
  return Copy;
}

/// Every `brk #0xf000` in the \p Size bytes at \p Begin, in address order.
///
/// Read at instruction alignment rather than byte by byte. A stride of one
/// could match the tail of one instruction against the head of the next and
/// report a trap that is not in the program at all; every arm64 instruction is
/// four bytes, so a stride of four sees each of them exactly once.
llvm::Expected<std::vector<lldb::addr_t>>
FindDebugTraps(Process &Proc, lldb::addr_t Begin, lldb::addr_t Size) {
  std::vector<uint8_t> Code(Size);
  Status ReadError;
  if (Proc.ReadMemory(Begin, Code.data(), Code.size(), ReadError) !=
      Code.size())
    return Refuse(PatchFailure::InferiorAccessFailed,
                  llvm::Twine("the compiled copy could not be read back: ") +
                      ReadError.AsCString());

  std::vector<lldb::addr_t> Traps;
  for (size_t Offset = 0; Offset + kInstructionSize <= Code.size();
       Offset += kInstructionSize)
    if (llvm::support::endian::read32le(Code.data() + Offset) ==
        kDebugTrapOpcode)
      Traps.push_back(Begin + Offset);
  return Traps;
}

/// The injections in the order the builder emits their code.
///
/// By line, and stably so that two on one line keep the order they were
/// installed in. This has to agree with \ref BuildPatchSource, because the
/// traps read back out of the copy are matched to injections by position: an
/// order that disagreed would credit one site with another's hits.
std::vector<const PatchInjection *>
InEmissionOrder(llvm::ArrayRef<PatchInjection> Injections) {
  std::vector<const PatchInjection *> Ordered;
  Ordered.reserve(Injections.size());
  for (const PatchInjection &Inj : Injections)
    Ordered.push_back(&Inj);
  std::stable_sort(Ordered.begin(), Ordered.end(),
                   [](const PatchInjection *A, const PatchInjection *B) {
                     return A->Line < B->Line;
                   });
  return Ordered;
}

/// How many traps the builder emits for \p Inj.
///
/// A stop trap only when the site wants a stop, and a drain trap only when it
/// records something. So the count is known before the copy is read, which is
/// what makes a copy holding a different number a disagreement rather than a
/// discovery.
size_t TrapsEmittedFor(const PatchInjection &Inj) {
  return (Inj.WantStop ? 1 : 0) + (Inj.Captures.empty() ? 0 : 1);
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
    Fresh->Name = Facts->Name;
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
      return Refuse(PatchFailure::InferiorAccessFailed,
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
      return Refuse(PatchFailure::InferiorAccessFailed,
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
    return Refuse(PatchFailure::InferiorAccessFailed,
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
    return Refuse(PatchFailure::InferiorAccessFailed,
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
      return Refuse(PatchFailure::InferiorAccessFailed,
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
    return Refuse(PatchFailure::InferiorAccessFailed,
                  llvm::Twine("a site's slot could not be cleared: ") +
                      WriteError.AsCString());

  return Slot;
}

llvm::Error FunctionPatchManager::Recompile(PatchedFunction &Fn) {
  Process *Proc = m_target.GetProcessSP().get();
  if (!Proc)
    return Refuse(PatchFailure::NoProcess);

  const uint32_t FirstLine = Fn.Body.FirstLine;
  const size_t LineCount = Fn.Body.LineStarts.size();
  const uint32_t LastLine =
      LineCount ? FirstLine + static_cast<uint32_t>(LineCount) - 1 : FirstLine;

  // The builder emits nothing for a line outside the body, since the
  // declaration line and the closing brace are not statements. Saying so is
  // better than installing a site whose code was never emitted and which could
  // therefore never fire.
  for (const PatchInjection &Inj : Fn.Injections)
    if (Inj.Line <= FirstLine || Inj.Line >= LastLine)
      return Refuse(PatchFailure::Unsupported,
                    llvm::Twine("line ") + llvm::Twine(Inj.Line) +
                        " holds no statement of the function's body, which "
                        "spans lines " +
                        llvm::Twine(FirstLine) + " to " +
                        llvm::Twine(LastLine));

  // Built from the original body every time rather than from the last copy,
  // which is what makes a second injection compose with the first instead of
  // patching a patch.
  PatchSourceRequest Request;
  Request.Body = Fn.Body;
  Request.SourcePath = Fn.SourcePath;
  Request.RingAddress = m_ring_address;
  Request.RingCapacity = kDefaultRingCapacity;
  Request.Injections = Fn.Injections;

  // Every compile is a fresh top-level expression whose declarations persist
  // in the target, so a tag of its own is what keeps this one from redefining
  // the last one's -- whether that was for this function or another, and
  // whether recompiling only dropped an injection rather than adding one.
  Request.Tag = std::to_string(m_next_compile_tag++);

  llvm::Expected<CompiledCopy> Copy =
      CompileCopy(m_target, BuildPatchSource(Request), Fn.Name);
  if (!Copy)
    return Copy.takeError();

  llvm::Expected<std::vector<lldb::addr_t>> Traps =
      FindDebugTraps(*Proc, Copy->Address, Copy->Size);
  if (!Traps)
    return Traps.takeError();

  const std::vector<const PatchInjection *> Ordered =
      InEmissionOrder(Fn.Injections);
  size_t TrapsExpected = 0;
  for (const PatchInjection *Inj : Ordered)
    TrapsExpected += TrapsEmittedFor(*Inj);
  if (Traps->size() != TrapsExpected)
    // Not a refusal. The source this read back is the source this built, so a
    // disagreement is between two halves of one implementation rather than
    // something about the program being patched -- and assigning the traps
    // anyway would credit hits to whichever site the miscount shifted them
    // onto.
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "the compiled copy holds %zu traps where its source emitted %zu",
        Traps->size(), TrapsExpected);

  // Registered before the module is announced, so that a `file:line` breakpoint
  // re-resolving into the copy finds these addresses already claimed by sites
  // that install nothing. Announcing first would let such a breakpoint write
  // its own trap over the instruction after ours, which would then stop on
  // every pass rather than only when the program's own trap executed.
  std::vector<std::pair<uint32_t, lldb::break_id_t>> Registered;
  auto DropRegistered = [&]() {
    for (const auto &[SiteID, BreakID] : Registered)
      m_target.RemoveBreakpointByID(BreakID);
  };
  size_t Next = 0;
  for (const PatchInjection *Inj : Ordered) {
    const PatchSiteCallback Callback = Fn.Callbacks.lookup(Inj->SiteID);
    // Within one injection the stop trap precedes the drain trap, which is the
    // order the builder writes them in. Only the stop trap carries the site's
    // callback: a drain is the debugger's own errand rather than a hit the
    // caller asked to hear about.
    if (Inj->WantStop) {
      llvm::Expected<lldb::break_id_t> BreakID =
          RegisterTrapSite((*Traps)[Next++], Callback.OnTrap, Callback.Baton);
      if (!BreakID) {
        DropRegistered();
        return BreakID.takeError();
      }
      Registered.emplace_back(Inj->SiteID, *BreakID);
    }
    if (!Inj->Captures.empty()) {
      llvm::Expected<lldb::break_id_t> BreakID =
          RegisterTrapSite((*Traps)[Next++], nullptr, nullptr);
      if (!BreakID) {
        DropRegistered();
        return BreakID.takeError();
      }
      Registered.emplace_back(Inj->SiteID, *BreakID);
    }
  }

  // Notified, not merely appended. Notification is what makes lldb re-resolve
  // `file:line` breakpoints into the copy, which is how a stop inside the copy
  // reports the file and line of the original rather than nothing at all. The
  // parse appended the module already, having generated debug info, but said
  // nothing about it.
  ModuleList JustTheCopy;
  JustTheCopy.Append(Copy->Module);
  m_target.GetImages().AppendIfNeeded(Copy->Module, /*notify=*/false);
  m_target.ModulesDidLoad(JustTheCopy);

  // Written last, and only now that the copy's address is known: until this
  // lands the copy is unreachable, so every refusal above leaves the program
  // running the code it was already running.
  const std::array<uint8_t, kEntryTrampolineSize> Trampoline =
      EncodeEntryTrampoline(Copy->Address);
  Status WriteError;
  if (Proc->WriteMemory(Fn.Entry, Trampoline.data(), Trampoline.size(),
                        WriteError) != Trampoline.size()) {
    DropRegistered();
    return Refuse(PatchFailure::InferiorAccessFailed,
                  llvm::Twine("the redirect could not be written over the "
                              "function's entry: ") +
                      WriteError.AsCString());
  }

  for (const auto &[SiteID, BreakID] : Registered)
    Fn.SiteBreakpoints[SiteID].push_back(BreakID);

  // The copy this one replaces is retired rather than freed: a thread already
  // inside it returns through it, and the expression that owns its code is what
  // keeps the module describing it in the target's images.
  if (Fn.CurrentModule)
    Fn.RetiredModules.push_back(std::move(Fn.CurrentModule));
  if (Fn.CurrentExpression)
    Fn.RetiredExpressions.push_back(std::move(Fn.CurrentExpression));
  Fn.CurrentModule = std::move(Copy->Module);
  Fn.CurrentExpression = std::move(Copy->Expression);
  Fn.CopyAddress = Copy->Address;
  return llvm::Error::success();
}

llvm::Expected<lldb::break_id_t>
FunctionPatchManager::RegisterTrapSite(lldb::addr_t Trap,
                                       BreakpointHitCallback OnTrap,
                                       const lldb::BatonSP &Baton) {
  Process *Proc = m_target.GetProcessSP().get();
  if (!Proc)
    return Refuse(PatchFailure::NoProcess);

  // On the trap rather than past it. A `brk` raises a mach exception naming the
  // trap's own address, and that address is what the stop is attributed by; pc
  // has already moved past the trap by the time the stop arrives, so a site
  // where pc points would never be the one looked for.
  Address SiteAddress;
  if (!m_target.ResolveLoadAddress(Trap, SiteAddress))
    return Refuse(PatchFailure::CompileFailed,
                  "the compiled copy's trap is in no loaded section");

  // A site is attributed through a breakpoint location, and the caller passed a
  // callback rather than a location, so each trap is given an internal
  // breakpoint of its own to carry that callback.
  lldb::BreakpointSP Bp = m_target.CreateBreakpoint(SiteAddress,
                                                    /*internal=*/true,
                                                    /*hardware=*/false);
  lldb::BreakpointLocationSP Loc = Bp ? Bp->GetLocationAtIndex(0) : nullptr;
  if (!Loc) {
    if (Bp)
      m_target.RemoveBreakpointByID(Bp->GetID());
    return Refuse(PatchFailure::CompileFailed,
                  "no breakpoint location could be made for the copy's trap");
  }
  Bp->SetBreakpointKind("in-process-condition");

  // Resolving that location installed a trap of its own here, which would fire
  // on every pass rather than only when the program's trap executed -- pc
  // reaches this address whenever a guard branched past the trap, too. Taking
  // it down leaves the instruction the compiler put here intact, and the
  // program-trap site that replaces it installs nothing at all.
  Bp->SetEnabled(false);
  if (Proc->CreateProgramTrapSite(Loc, SiteAddress.GetLoadAddress(&m_target)) ==
      LLDB_INVALID_BREAK_ID) {
    m_target.RemoveBreakpointByID(Bp->GetID());
    return Refuse(PatchFailure::BreakpointInPatchRange,
                  "at the address the copy's trap reports at");
  }
  // Enabled again with the site already in place, so nothing is written: an
  // enabled location is what lets the stop be attributed and the callback run.
  Bp->SetEnabled(true);

  if (OnTrap)
    Bp->SetCallback(OnTrap, Baton, /*is_synchronous=*/false);

  return Bp->GetID();
}
