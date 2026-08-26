//===-- FunctionPatch.cpp -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/FunctionPatch.h"
#include "lldb/Breakpoint/Breakpoint.h"
#include "lldb/Breakpoint/BreakpointList.h"
#include "lldb/Breakpoint/BreakpointLocation.h"
#include "lldb/Breakpoint/BreakpointResolver.h"
#include "lldb/Breakpoint/BreakpointSite.h"
#include "lldb/Core/Address.h"
#include "lldb/Core/AddressRange.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleList.h"
#include "lldb/Core/SourceManager.h"
#include "lldb/Expression/DiagnosticManager.h"
#include "lldb/Expression/ExpressionVariable.h"
#include "lldb/Expression/LLVMUserExpression.h"
#include "lldb/Expression/UserExpression.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Symbol/Block.h"
#include "lldb/Symbol/CompilerDeclContext.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/Type.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/EntryTrampoline.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"
#include "lldb/Target/ThreadList.h"
#include "lldb/Utility/Baton.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/State.h"
#include "lldb/Utility/SupportFile.h"
#include "lldb/ValueObject/ValueObjectConstResult.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/TargetParser/Triple.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
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

/// Records the debugger holds for a caller that has not asked for them yet.
///
/// Bounded for the same reason the ring in the inferior is: a hot site whose
/// values nobody collects would otherwise cost the debugger memory without end.
/// Sixteen bytes a record, so this is sixteen megabytes at worst.
constexpr size_t kMaxPendingRecords = 1 << 20;

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

  /// The internal breakpoints that carry each live site's callback, one per
  /// trap the current copy contains for that site.
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

  /// The sites that attribute the traps those copies still contain, under the
  /// site each belongs to. Left registered, because a thread already inside a
  /// retired copy runs its traps, and a trap with no site to attribute it
  /// surfaces as a bare exception. Kept per site so that removing an injection
  /// can reach every trap of it that is still in the program's text.
  llvm::DenseMap<uint32_t, std::vector<lldb::break_id_t>> RetiredSites;
};

} // namespace lldb_private

llvm::StringRef lldb_private::ToString(PatchFailure Reason) {
  switch (Reason) {
  case PatchFailure::NotArm64:
    return "in-process evaluation is implemented for arm64 only";
  case PatchFailure::NoProcess:
    return "there is no process this can be patched into right now";
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
  case PatchFailure::BreakpointInRedirectedBody:
    return "a breakpoint in the function would stop firing once its entry is "
           "redirected to a copy";
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

llvm::SmallVector<uint8_t, 8>
lldb_private::CaptureValueBytes(uint64_t Value, size_t ByteSize,
                                lldb::ByteOrder Order) {
  // Clamped rather than trusted. A type wider than the field cannot have fitted
  // through it, so a width that says otherwise would only read past the record.
  const size_t Width = std::min<size_t>(ByteSize, sizeof(uint64_t));
  llvm::SmallVector<uint8_t, 8> Bytes(Width, 0);
  for (size_t I = 0; I < Width; ++I) {
    const uint8_t Byte = static_cast<uint8_t>(Value >> (8 * I));
    Bytes[Order == lldb::eByteOrderBig ? Width - 1 - I : I] = Byte;
  }
  return Bytes;
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

/// Whether the function entered at \p Entry can have its body made unreachable
/// without a breakpoint quietly going with it.
///
/// A redirected entry means the original body never runs again, so a breakpoint
/// location in that body never traps again. Announcing the copy re-resolves
/// breakpoints into it, but only a resolver that goes by file and line finds
/// it: the copy's line table names the original source, while the compile
/// unit's primary file -- which is the only text a source-regex resolver
/// searches -- is the generated source the copy was compiled from. Every other
/// resolver is treated as not finding the copy, since being wrong that way
/// costs a patch and being wrong the other way costs somebody's breakpoint.
///
/// A refusal here leaves the expression to be evaluated at a stop, which is
/// slower and says so. Installing anyway would leave a breakpoint that reads as
/// resolved and never fires again, which nothing says at all.
llvm::Error CheckNoBreakpointNeedsTheOriginalBody(Target &Tgt,
                                                  lldb::addr_t Entry,
                                                  lldb::break_id_t Carried) {
  std::string Orphaned;
  size_t Count = 0;

  // Internal breakpoints as well as the user's. One of those standing for a
  // thread plan's next step would fail the same way, and a step that never
  // arrives is no easier to explain than a breakpoint that never fires.
  for (bool Internal : {false, true}) {
    for (const lldb::BreakpointSP &Bp :
         Tgt.GetBreakpointList(Internal).Breakpoints()) {
      // The breakpoint the injection is being installed for. Its locations
      // stop trapping and its hits arrive from the trap instead, which is the
      // whole of what the injection is for.
      if (Bp->GetID() == Carried)
        continue;

      lldb::BreakpointResolverSP Resolver = Bp->GetResolver();
      if (Resolver && Resolver->getResolverID() ==
                          BreakpointResolver::ResolverTy::FileLineResolver)
        continue;

      // Facade locations stand for code rather than sitting at any, so the real
      // locations are the ones that could stop trapping.
      const size_t Locations = Bp->GetNumLocations(/*use_facade=*/false);
      for (size_t I = 0; I < Locations; ++I) {
        lldb::BreakpointLocationSP Loc =
            Bp->GetLocationAtIndex(I, /*use_facade=*/false);
        if (!Loc || !Loc->IsEnabled())
          continue;

        // A location carrying a condition is one this same mechanism is asked
        // about at every stop: it either gets an injection in the copy or
        // records why it could not. Accounted for either way, so losing its own
        // trap is not silent.
        if (Loc->GetCondition())
          continue;

        // Asked of the location's own function rather than of an address range,
        // so that a function whose code is in several pieces is answered for
        // all of them -- every piece is reached through the entry that is about
        // to stop being reached.
        Function *Enclosing =
            Loc->GetAddress().CalculateSymbolContextFunction();
        if (!Enclosing || Enclosing->GetAddress().GetLoadAddress(&Tgt) != Entry)
          continue;

        if (Count++)
          Orphaned += ", ";
        Orphaned += std::to_string(Bp->GetID());
        Orphaned += ".";
        Orphaned += std::to_string(Loc->GetID());
      }
    }
  }

  if (!Count)
    return llvm::Error::success();

  // Named, because the only way past this refusal is to take the breakpoint
  // that stands in the way off the function, and a reason that does not say
  // which breakpoint that is cannot be acted on.
  return Refuse(PatchFailure::BreakpointInRedirectedBody,
                llvm::Twine(Count == 1 ? "breakpoint " : "breakpoints ") +
                    Orphaned);
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

  /// What describes the copy's own locals, which is where each capture's type
  /// is read from. Owned by \ref Module, so it lives exactly as long.
  Function *Definition = nullptr;
};

/// Takes \p Name back out of the declarations the target remembers.
///
/// A top-level expression's declarations persist so that a later expression can
/// name them. A copy is compiled as a top-level definition and deliberately
/// carries the original's name, so what persists is a definition of a name the
/// program already has: the next compile of the same function finds it and is a
/// redefinition of it, and a user expression naming the function finds the
/// debugger's copy of it rather than the program's own.
void ForgetCopyDeclaration(Target &Tgt, ConstString Name) {
  if (PersistentExpressionState *State =
          Tgt.GetPersistentExpressionStateForLanguage(lldb::eLanguageTypeC))
    State->ForgetPersistentDecl(Name);
}

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
  const bool Parsed = Expr->Parse(Diagnostics, ExeCtx, eExecutionPolicyTopLevel,
                                  /*keep_result_in_memory=*/true,
                                  /*generate_debug_info=*/true);

  // Whatever the parse recorded is recorded whether or not it went on to
  // succeed, so this is undone here rather than only on the way out.
  ForgetCopyDeclaration(Tgt, Name);

  if (!Parsed)
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
  Copy.Definition = CopyFunction;
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

/// What a site whose injection has been removed does when its trap still fires.
///
/// The trap is in a copy a thread may be running inside, so it cannot be taken
/// back out, and nobody is waiting for it any more. Resuming without reporting
/// is the only outcome left; it beats unregistering the site, which would let
/// the trap arrive as an exception with nothing to explain it.
bool ResumeWithoutReporting(void *, StoppointCallbackContext *, lldb::user_id_t,
                            lldb::user_id_t) {
  return false;
}

/// The type the copy's debug info gives the local named \p Name, or an invalid
/// type when the copy declares no such local.
///
/// Searched through the function's child blocks as well as its own, because an
/// injection inside a loop or an `if` puts its locals in the block that
/// statement opened rather than at the function's top level.
CompilerType LocalType(Function &Copy, llvm::StringRef Name) {
  const ConstString Wanted(Name);
  VariableList Locals;
  Copy.GetBlock(/*can_create=*/true)
      .AppendBlockVariables(
          /*can_create=*/true, /*get_child_block_variables=*/true,
          /*stop_if_child_block_is_inlined_function=*/false,
          [Wanted](Variable *Var) { return Var && Var->GetName() == Wanted; },
          &Locals);
  if (Locals.GetSize() == 0)
    return CompilerType();

  lldb::VariableSP Local = Locals.GetVariableAtIndex(0);
  Type *Declared = Local ? Local->GetType() : nullptr;
  return Declared ? Declared->GetForwardCompilerType() : CompilerType();
}

/// Why a value of \p Type cannot travel through the record's eight-byte field,
/// or an empty string when it can.
///
/// The gate the whole capture path depends on. The field is fixed, so a type
/// wider than it never arrived intact, and a type that is not a scalar has
/// no single value the field could have held.
std::string WhyNotRecordable(const CompilerType &Type) {
  if (!Type.IsValid())
    return "the compiled copy's debug info describes no type for it";
  if (!Type.IsScalarType())
    return ("\"" + Type.GetDisplayTypeName().GetStringRef() +
            "\" is not a scalar")
        .str();

  llvm::Expected<uint64_t> ByteSize = Type.GetByteSize(/*exe_scope=*/nullptr);
  if (!ByteSize) {
    llvm::consumeError(ByteSize.takeError());
    return ("the size of \"" + Type.GetDisplayTypeName().GetStringRef() +
            "\" is not known")
        .str();
  }
  if (*ByteSize == 0 || *ByteSize > sizeof(uint64_t))
    return llvm::formatv("\"{0}\" occupies {1} bytes",
                         Type.GetDisplayTypeName().GetStringRef(), *ByteSize)
        .str();
  return "";
}

/// What a drain trap does: read what the ring holds, and let the program carry
/// on.
///
/// A drain is the debugger's own errand rather than a hit anyone asked to hear
/// about, so nothing is reported. This fires with nothing to read more often
/// than not -- threads keep running until the stop is delivered, and the trap
/// sits outside the per-site guards -- so finding the ring empty is the
/// ordinary outcome rather than a problem.
bool DrainAndResume(void *Baton, StoppointCallbackContext *, lldb::user_id_t,
                    lldb::user_id_t) {
  if (auto *Manager = static_cast<FunctionPatchManager *>(Baton))
    if (llvm::Error Err = Manager->CollectRecords())
      LLDB_LOG_ERROR(GetLog(LLDBLog::Breakpoints), std::move(Err),
                     "recorded values could not be read back: {0}");
  return false;
}

} // namespace

FunctionPatchManager::FunctionPatchManager(Target &Tgt) : m_target(Tgt) {}

FunctionPatchManager::~FunctionPatchManager() = default;

llvm::Expected<uint32_t>
FunctionPatchManager::Install(const PatchRequest &Request) {
  // The redirect is hand-encoded arm64 and nothing else has an encoding.
  if (m_target.GetArchitecture().GetTriple().getArch() != llvm::Triple::aarch64)
    return Refuse(PatchFailure::NotArm64);

  // Held still and past the loader's startup, rather than merely alive:
  // installing rewrites the inferior's code and reads every thread's PC, and a
  // copy the loader is about to forget describes nothing anyone can use.
  if (!m_target.CanCompileCodeIntoProcess())
    return Refuse(PatchFailure::NoProcess);
  Process *Proc = m_target.GetProcessSP().get();

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

  // Only while the entry still reaches the original body. Once it reaches a
  // copy instead, the body has already stopped being run, and a refusal now
  // would neither have caused that nor undo it.
  if (Fn->CopyAddress == LLDB_INVALID_ADDRESS)
    if (llvm::Error Err = CheckNoBreakpointNeedsTheOriginalBody(
            m_target, Entry, Request.HitsCarriedBy))
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
    m_captures.erase(SiteID);
    m_dropped_captures.erase(SiteID);
    return std::move(Err);
  }

  if (Fresh)
    m_functions[Entry] = std::move(Fresh);
  m_site_to_function[SiteID] = Entry;
  // Recorded once the site is live, because this is what makes its counters
  // readable and there are none to read for a site that was refused.
  m_site_slots[SiteID] = *Slot;

  // Every site's counters, like every record in the ring, are only readable
  // while the process holding them is there, and a run whose condition never
  // held takes no other stop at which they could be read.
  EnsureExitDrainBreakpoint();
  return SiteID;
}

llvm::Error FunctionPatchManager::Remove(uint32_t SiteID) {
  auto SiteIt = m_site_to_function.find(SiteID);
  if (SiteIt == m_site_to_function.end())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "site %u has not been installed", SiteID);
  auto FnIt = m_functions.find(SiteIt->second);
  if (FnIt == m_functions.end())
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "site %u names a function that is not patched", SiteID);
  PatchedFunction &Fn = *FnIt->second;

  // Silenced rather than unregistered, which is all removal can do for a trap
  // the program's text still holds: a thread already inside one of these copies
  // reaches it, and a stop nobody wants is a better outcome than one nothing
  // can explain. Every copy that holds this injection's trap, not only the
  // current one, since an earlier copy a thread is still inside holds it too.
  for (lldb::break_id_t BreakID : Fn.SiteBreakpoints.lookup(SiteID))
    SilenceSite(BreakID);
  for (lldb::break_id_t BreakID : Fn.RetiredSites.lookup(SiteID))
    SilenceSite(BreakID);

  llvm::erase_if(Fn.Injections, [SiteID](const PatchInjection &Inj) {
    return Inj.SiteID == SiteID;
  });
  Fn.Callbacks.erase(SiteID);
  m_site_to_function.erase(SiteIt);

  // The site's counters go with it. Its slot's address would still read, but a
  // count reported for a site the caller has taken away is a count for nothing.
  m_site_slots.erase(SiteID);
  m_counters.erase(SiteID);
  m_captures.erase(SiteID);
  m_dropped_captures.erase(SiteID);

  // Recompiled even when nothing is left to inject, rather than putting the
  // original entry back. Restoring it means writing all sixteen bytes of a
  // trampoline threads are branching through, where re-pointing the literal at
  // a fresh copy is one aligned store that no thread can observe half of.
  return Recompile(Fn);
}

void FunctionPatchManager::SilenceSite(lldb::break_id_t BreakID) {
  if (lldb::BreakpointSP Bp = m_target.GetBreakpointByID(BreakID))
    Bp->SetCallback(ResumeWithoutReporting, lldb::BatonSP(),
                    /*is_synchronous=*/false);
}

void FunctionPatchManager::SetGate(uint32_t SiteID, bool Open) {
  auto Slot = m_site_slots.find(SiteID);
  if (Slot == m_site_slots.end())
    return;
  Process *Proc = m_target.GetProcessSP().get();
  if (!Proc)
    return;

  // One byte, so a site reads either the old state or the new one and never
  // half of each: the injected code loads the gate on a path the debugger
  // cannot hold still, since the thread that opens a gate is not the only
  // thread that may be running the patched function.
  const uint8_t Byte = Open ? 1 : 0;
  Status WriteError;
  if (Proc->WriteMemory(Slot->second + offsetof(PatchSiteSlot, Gate), &Byte,
                        sizeof(Byte), WriteError) == sizeof(Byte))
    return;

  // Logged rather than returned. A gate that could not be written leaves the
  // site doing what it was doing before, which for a gate being opened is
  // nothing and for one being closed is too much; either way the caller's own
  // per-hit test still decides what is recorded, so this costs accuracy in the
  // hit count rather than correctness in what is reported.
  LLDB_LOG(GetLog(LLDBLog::Breakpoints),
           "site {0}'s gate could not be {1}: {2}", SiteID,
           Open ? "opened" : "closed", WriteError.AsCString());
}

void FunctionPatchManager::ForgetProcess() {
  // Dropping a patched function releases the expression its copy's code belongs
  // to, which is what takes the module describing that copy back out of the
  // target's images.
  m_functions.clear();
  m_site_to_function.clear();
  m_ring_address = LLDB_INVALID_ADDRESS;
  m_slot_pool = LLDB_INVALID_ADDRESS;
  m_slots_used_in_page = 0;
  m_next_site_id = 1;

  // A record still held is a record that can no longer be turned into a value:
  // its type lived in the copy's debug info, which has just been dropped along
  // with the copy. Counted as lost rather than discarded, so the next caller is
  // told how much never reached it instead of reading a short list as a whole
  // one.
  m_pending_lost += m_pending.size();
  m_pending.clear();
  m_captures.clear();
  m_dropped_captures.clear();
  m_site_slots.clear();
  m_counters.clear();
}

llvm::ArrayRef<std::string>
FunctionPatchManager::GetDroppedCaptures(uint32_t SiteID) const {
  auto It = m_dropped_captures.find(SiteID);
  if (It == m_dropped_captures.end())
    return {};
  return It->second;
}

void FunctionPatchManager::EnsureExitDrainBreakpoint() {
  // Asked once. A second attempt that failed the first time would fail the same
  // way, and one that succeeded would set a second breakpoint on the same code.
  if (m_exit_drain != LLDB_INVALID_BREAK_ID || !m_tail_drain_refusal.empty())
    return;

  // Both names, since which one ends the program is libc's business rather than
  // anything the debugger can see from here, and stopping at both costs one
  // extra read of an empty ring.
  const std::vector<std::string> Names = {"exit", "_exit"};
  lldb::BreakpointSP Bp = m_target.CreateBreakpoint(
      /*containingModules=*/nullptr, /*containingSourceFiles=*/nullptr, Names,
      lldb::eFunctionNameTypeFull, lldb::eLanguageTypeUnknown, /*m_offset=*/0,
      eLazyBoolNo, /*internal=*/true, /*request_hardware=*/false);

  if (Bp && Bp->GetNumLocations() > 0) {
    Bp->SetBreakpointKind("patch-drain-exit");
    // The manager itself is the baton, and an untyped one so that the
    // callback's lifetime says nothing about the manager's: the target owns
    // both, and the callback only runs while a process of that target is
    // stopped.
    Bp->SetCallback(DrainAndResume, std::make_shared<UntypedBaton>(this),
                    /*is_synchronous=*/false);
    m_exit_drain = Bp->GetID();
    return;
  }

  if (Bp)
    m_target.RemoveBreakpointByID(Bp->GetID());

  // Kept rather than only logged, and not a refusal of the install: recording
  // still works, and a run long enough to fill the ring still reports. What is
  // lost is the tail of a short run, and a caller told nothing about it would
  // read a run whose values never arrived as a run that recorded none.
  m_tail_drain_refusal =
      "neither \"exit\" nor \"_exit\" resolved, so a run that takes no other "
      "stop reaches its end with its recorded values and its hit counts still "
      "inside the program";
  LLDB_LOG(GetLog(LLDBLog::Breakpoints), "{0}", m_tail_drain_refusal);
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
  //
  // Compiled more than once when a capture turns out to be unrecordable, since
  // `__typeof__` is what decides a capture's type and only the compiler knows
  // what that came to. Each pass drops at least one capture, so the number of
  // passes is bounded by the number of captures.
  std::optional<CompiledCopy> Copy;
  while (!Copy) {
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

    llvm::Expected<CompiledCopy> Compiled =
        CompileCopy(m_target, BuildPatchSource(Request), Fn.Name);
    if (!Compiled)
      return Compiled.takeError();

    // A copy that records something it should not is discarded here rather than
    // installed: nothing points at it yet, so letting go of it leaves the
    // program running exactly the code it was already running.
    if (!RecordCaptureTypes(Fn, *Compiled->Definition, Request.Tag))
      Copy = std::move(*Compiled);
  }

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
    // Assigning them anyway would credit hits to whichever site the miscount
    // shifted them onto, so this refuses instead. Two ways to get here, and the
    // count cannot tell them apart: the body itself contains a `brk #0xf000` --
    // `__builtin_debugtrap()` in the program's own source is the only ordinary
    // way -- or the builder and this reader disagree about what was emitted,
    // which is two halves of one implementation falling out of step.
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "the compiled copy holds %zu debug traps where its source emitted %zu, "
        "so a trap cannot be told from one the function's own code contains",
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
    // order the builder writes them in. The stop trap carries the site's own
    // callback; the drain trap carries the debugger's, since reading the ring
    // is the debugger's errand rather than a hit the caller asked to hear
    // about.
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
          RegisterTrapSite((*Traps)[Next++], DrainAndResume,
                           std::make_shared<UntypedBaton>(this));
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
  //
  // A function already redirected has its trampoline's literal re-pointed
  // rather than the whole trampoline rewritten. The literal is one aligned
  // eight-byte store, so a thread running the two instructions above it reads
  // one target or the other; rewriting all sixteen bytes would leave a window
  // in which a half-written redirect was reachable.
  const std::array<uint8_t, kEntryTrampolineSize> Trampoline =
      EncodeEntryTrampoline(Copy->Address);
  const bool AlreadyRedirected = Fn.CopyAddress != LLDB_INVALID_ADDRESS;
  const size_t WriteOffset =
      AlreadyRedirected ? kEntryTrampolineTargetOffset : 0;
  const size_t WriteSize = Trampoline.size() - WriteOffset;
  Status WriteError;
  if (Proc->WriteMemory(Fn.Entry + WriteOffset, Trampoline.data() + WriteOffset,
                        WriteSize, WriteError) != WriteSize) {
    DropRegistered();

    // A write that reported less than it was asked for may have landed in part,
    // which for these bytes means an entry that branches to nothing anyone
    // compiled. What was there is known -- the original instructions before the
    // first patch, and the literal naming the copy still in the program after
    // one -- so it is put back rather than left as the failure found it.
    const std::array<uint8_t, kEntryTrampolineSize> Previous =
        AlreadyRedirected ? EncodeEntryTrampoline(Fn.CopyAddress)
                          : Fn.OriginalBytes;
    Status RestoreError;
    const bool Restored =
        Proc->WriteMemory(Fn.Entry + WriteOffset, Previous.data() + WriteOffset,
                          WriteSize, RestoreError) == WriteSize;
    // A string rather than a Twine held in a variable: a Twine refers to the
    // pieces it was built from, which here are temporaries of this statement.
    const std::string Detail =
        std::string("the redirect could not be written over the function's "
                    "entry: ") +
        WriteError.AsCString();
    if (!Restored)
      // Said as loudly as prose can: every other refusal here leaves the program
      // running the code it was already running, and this one may not have.
      return Refuse(PatchFailure::InferiorAccessFailed,
                    Detail + "; nor could the bytes it overwrote be put back (" +
                        RestoreError.AsCString() +
                        "), so the entry may hold part of a redirect and the "
                        "program can no longer be trusted to be the one that "
                        "was built");
    return Refuse(PatchFailure::InferiorAccessFailed, Detail);
  }

  // The sites of the copy this one replaces are retired rather than
  // unregistered, for the same reason its module is kept: a thread already
  // inside that copy still runs its traps, and a trap whose site has gone
  // surfaces as a bare exception instead of as the hit it is.
  for (const auto &Live : Fn.SiteBreakpoints) {
    std::vector<lldb::break_id_t> &Retired = Fn.RetiredSites[Live.first];
    Retired.insert(Retired.end(), Live.second.begin(), Live.second.end());
  }
  Fn.SiteBreakpoints.clear();
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

bool FunctionPatchManager::RecordCaptureTypes(PatchedFunction &Fn,
                                              Function &Copy,
                                              llvm::StringRef Tag) {
  bool Dropped = false;
  for (PatchInjection &Inj : Fn.Injections) {
    std::vector<RecordedCapture> Recorded;
    std::vector<std::string> Kept;
    for (uint32_t I = 0; I < Inj.Captures.size(); ++I) {
      const CompilerType Type =
          LocalType(Copy, CaptureLocalName(Tag, Inj.SiteID, I));
      const std::string Why = WhyNotRecordable(Type);
      if (!Why.empty()) {
        // Dropped on its own rather than refusing the injection, because one
        // aggregate in a capture list must not cost the caller every scalar
        // beside it. Kept for as long as the site, since a capture that never
        // arrives has to be explainable whenever the caller asks -- and not
        // cleared by the recompile it causes, which is the pass that no longer
        // has the capture to refuse.
        m_dropped_captures[Inj.SiteID].push_back(
            llvm::formatv("\"{0}\": {1}: {2}", Inj.Captures[I],
                          ToString(PatchFailure::CaptureNotScalar), Why)
                .str());
        Dropped = true;
        continue;
      }
      Recorded.push_back(RecordedCapture{Type, Inj.Captures[I]});
      Kept.push_back(Inj.Captures[I]);
    }
    // Replaced whole rather than updated in place. A capture's number is its
    // position in the list that survives, so dropping one renumbers those after
    // it and anything left over from before would answer for the wrong capture.
    m_captures[Inj.SiteID] = std::move(Recorded);
    Inj.Captures = std::move(Kept);
  }
  return Dropped;
}

llvm::Error FunctionPatchManager::CollectRecords() {
  // Nothing has ever been recorded, so there is nothing to read and no block to
  // read it from.
  if (m_ring_address == LLDB_INVALID_ADDRESS)
    return llvm::Error::success();

  Process *Proc = m_target.GetProcessSP().get();
  // Held still rather than merely alive: most of the stops this reads at are
  // stops the debugger takes and resumes itself -- a drain trap is one -- where
  // the process reads as running while its threads are not.
  if (!Proc || !m_target.IsProcessHeldStill())
    return Refuse(PatchFailure::NoProcess);

  uint8_t HeaderBytes[kPatchRingHeaderSize] = {};
  Status ReadError;
  if (Proc->ReadMemory(m_ring_address, HeaderBytes, sizeof(HeaderBytes),
                       ReadError) != sizeof(HeaderBytes))
    return Refuse(PatchFailure::InferiorAccessFailed,
                  llvm::Twine("the record block's header could not be read: ") +
                      ReadError.AsCString());

  // Read a field at a time in the inferior's byte order rather than by copying
  // into the struct, so that what is read is the layout stated here rather than
  // however the debugger's host happens to lay the same fields out.
  PatchRingHeader Header;
  Header.Seq = llvm::support::endian::read64le(HeaderBytes +
                                               offsetof(PatchRingHeader, Seq));
  Header.Drained = llvm::support::endian::read64le(
      HeaderBytes + offsetof(PatchRingHeader, Drained));
  Header.Capacity = llvm::support::endian::read64le(
      HeaderBytes + offsetof(PatchRingHeader, Capacity));
  Header.HighWater = llvm::support::endian::read64le(
      HeaderBytes + offsetof(PatchRingHeader, HighWater));

  // Skipped when the header says the inferior has written nothing new, which is
  // the ordinary case: a drain trap fires with nothing to read more often than
  // not, and every stop asks. Reading the ring anyway would cost 64KB for a
  // question the header already answered.
  if (Header.Seq > Header.Drained) {
    // The whole ring rather than the part the header claims is new. The header
    // came from a live process, so a torn one must not decide how much memory
    // to read; the drain tolerates a ring shorter than the capacity claims and
    // reports what it could not reach.
    std::vector<uint8_t> RingBytes(kDefaultRingCapacity * kPatchRecordSize);
    if (Proc->ReadMemory(m_ring_address + kPatchRingHeaderSize,
                         RingBytes.data(), RingBytes.size(),
                         ReadError) != RingBytes.size())
      return Refuse(PatchFailure::InferiorAccessFailed,
                    llvm::Twine("the record ring could not be read: ") +
                        ReadError.AsCString());

    const PatchDrain Drain = DrainPatchRing(Header, RingBytes);
    m_pending.insert(m_pending.end(), Drain.Records.begin(),
                     Drain.Records.end());
    m_pending_lost += Drain.Lost;

    // Overflowing the backlog loses the oldest records, which is the direction
    // the ring itself loses in, and counts them rather than dropping them
    // quietly.
    if (m_pending.size() > kMaxPendingRecords) {
      const size_t Excess = m_pending.size() - kMaxPendingRecords;
      m_pending.erase(m_pending.begin(), m_pending.begin() + Excess);
      m_pending_lost += Excess;
    }

    // Written back only once the records are in hand, so a read that failed
    // leaves the inferior believing they are still unread rather than letting
    // the next write overwrite them.
    uint8_t DrainedBytes[sizeof(uint64_t)] = {};
    llvm::support::endian::write64le(DrainedBytes, Drain.NewDrained);
    Status WriteError;
    if (Proc->WriteMemory(m_ring_address + offsetof(PatchRingHeader, Drained),
                          DrainedBytes, sizeof(DrainedBytes),
                          WriteError) != sizeof(DrainedBytes))
      return Refuse(PatchFailure::InferiorAccessFailed,
                    llvm::Twine("the count of records read could not be "
                                "written back: ") +
                        WriteError.AsCString());
  }

  // Read here rather than when a caller asks, because a run's final counters
  // are only readable while the process holding them is still there.
  for (const auto &[SiteID, Slot] : m_site_slots) {
    uint8_t SlotBytes[kPatchSiteSlotSize] = {};
    if (Proc->ReadMemory(Slot, SlotBytes, sizeof(SlotBytes), ReadError) !=
        sizeof(SlotBytes))
      return Refuse(PatchFailure::InferiorAccessFailed,
                    llvm::Twine("a site's counters could not be read: ") +
                        ReadError.AsCString());
    SiteCounters &Counters = m_counters[SiteID];
    Counters.Hits = llvm::support::endian::read64le(
        SlotBytes + offsetof(PatchSiteSlot, Hits));
    Counters.CondTrue = llvm::support::endian::read64le(
        SlotBytes + offsetof(PatchSiteSlot, CondTrue));
  }

  return llvm::Error::success();
}

llvm::Expected<PatchDrainResult> FunctionPatchManager::Drain() {
  // Read anything the debugger's own drains have not already taken, so that a
  // caller which only ever calls this still sees every record.
  if (llvm::Error Err = CollectRecords()) {
    // Nothing is being handed over, so the reason is all there is to hand over.
    if (m_pending.empty() && m_counters.empty())
      return std::move(Err);
    // A caller cannot be given both its values and a reason, and of the two
    // only the values cannot be had again: the reason is in the log, and the
    // next call will run into it afresh.
    LLDB_LOG_ERROR(GetLog(LLDBLog::Breakpoints), std::move(Err),
                   "recorded values were read back incompletely: {0}");
  }

  PatchDrainResult Result;
  Result.Counters = m_counters;
  Result.Lost = m_pending_lost;
  m_pending_lost = 0;

  Process *Proc = m_target.GetProcessSP().get();
  // The process while there is one, since that is what a value's own address
  // size and byte order come from, and the target once it has gone -- a record
  // read at the exit stop is still owed to whoever asks next.
  ExecutionContextScope *Scope =
      Proc ? static_cast<ExecutionContextScope *>(Proc) : &m_target;
  const lldb::ByteOrder Order = m_target.GetArchitecture().GetByteOrder();
  const uint32_t AddrSize = m_target.GetArchitecture().GetAddressByteSize();

  Result.Values.reserve(m_pending.size());
  for (const PatchRecord &Rec : m_pending) {
    auto Known = m_captures.find(Rec.Site);
    if (Known == m_captures.end() || Rec.Capture >= Known->second.size()) {
      // Written by a copy whose capture the current one no longer has, or by a
      // site since removed. Counted rather than passed over, because a value
      // nobody can name is a value that did not reach the caller.
      ++Result.Lost;
      continue;
    }
    const RecordedCapture &Capture = Known->second[Rec.Capture];

    llvm::Expected<uint64_t> ByteSize = Capture.Type.GetByteSize(Scope);
    if (!ByteSize) {
      llvm::consumeError(ByteSize.takeError());
      ++Result.Lost;
      continue;
    }

    // The type's own width, not the field's. The inferior copied the value into
    // eight bytes, so reading all eight back would report the padding beside a
    // `char` as part of its value.
    const llvm::SmallVector<uint8_t, 8> Bytes =
        CaptureValueBytes(Rec.Value, *ByteSize, Order);
    const DataExtractor Data(Bytes.data(), Bytes.size(), Order, AddrSize);
    lldb::ValueObjectSP Value = ValueObjectConstResult::Create(
        Scope, Capture.Type, ConstString(Capture.Expression), Data);
    if (!Value) {
      ++Result.Lost;
      continue;
    }
    Result.Values.push_back(
        CapturedValue{Rec.Site, Rec.Capture, std::move(Value)});
  }
  m_pending.clear();
  return Result;
}

llvm::Expected<lldb::break_id_t>
FunctionPatchManager::RegisterTrapSite(lldb::addr_t Trap,
                                       BreakpointHitCallback OnTrap,
                                       const lldb::BatonSP &Baton) {
  Process *Proc = m_target.GetProcessSP().get();
  if (!Proc)
    return Refuse(PatchFailure::NoProcess);

  // On the trap rather than past it. A `brk` raises a mach exception naming the
  // trap's own address, and that address is what the stop is attributed by. pc
  // is on the trap too when the stop arrives, which is why the stop has to
  // advance it: a trap in the program's own code has no opcode to lift and put
  // back, so nothing else would.
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
