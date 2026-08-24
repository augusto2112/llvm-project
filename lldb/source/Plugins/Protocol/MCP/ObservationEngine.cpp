//===- ObservationEngine.cpp ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ObservationEngine.h"
#include "SerializeValue.h"
#include "ValueObjectNode.h"
#include "lldb/Breakpoint/Breakpoint.h"
#include "lldb/Breakpoint/StoppointCallbackContext.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/SourceManager.h"
#include "lldb/Host/Host.h"
#include "lldb/Host/PosixApi.h"
#include "lldb/Host/ProcessLaunchInfo.h"
#include "lldb/Symbol/CompilerType.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/LineEntry.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/ABI.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/TargetList.h"
#include "lldb/Target/Thread.h"
#include "lldb/Utility/Environment.h"
#include "lldb/Utility/FileSpec.h"
#include "lldb/Utility/Listener.h"
#include "lldb/Utility/State.h"
#include "lldb/Utility/Status.h"
#include "lldb/Utility/StreamString.h"
#include "lldb/ValueObject/ValueObject.h"
#include "lldb/lldb-enumerations.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace lldb_private;
using namespace lldb_private::mcp;
using namespace llvm;

using Clock = std::chrono::steady_clock;
using Micros = std::chrono::microseconds;

namespace {

double ToMs(Micros D) { return static_cast<double>(D.count()) / 1000.0; }

/// Rounds to hundredths, so that a measurement does not spend twelve characters
/// asserting a precision it does not have.
double Round2(double Value) { return std::round(Value * 100.0) / 100.0; }

std::string Compact(const json::Value &V) {
  std::string S;
  raw_string_ostream OS(S);
  OS << V;
  return S;
}

} // namespace

//===----------------------------------------------------------------------===//
// Emission
//===----------------------------------------------------------------------===//

StringRef lldb_private::mcp::ToString(Outcome O) {
  switch (O) {
  case Outcome::Exited:
    return "exited";
  case Outcome::Crashed:
    return "crashed";
  case Outcome::TimedOut:
    return "timed_out";
  case Outcome::NoProgress:
    return "no_progress";
  }
  llvm_unreachable("unhandled Outcome");
}

bool lldb_private::mcp::IsAbnormal(Outcome O) { return O != Outcome::Exited; }

StringRef lldb_private::mcp::ToString(EmitDecision D) {
  switch (D) {
  case EmitDecision::Emit:
    return "emit";
  case EmitDecision::Skip:
    return "skip";
  case EmitDecision::Hold:
    return "hold";
  }
  llvm_unreachable("unhandled EmitDecision");
}

EmitDecision lldb_private::mcp::DecideEmit(const EmitDecisionInput &In) {
  // Selecting one hit overrides the mode, which has nothing left to reduce.
  if (In.OnlyHit) {
    const uint64_t Absolute = static_cast<uint64_t>(In.SkipFirst) + In.HitIndex;
    return Absolute == *In.OnlyHit ? EmitDecision::Emit : EmitDecision::Skip;
  }

  switch (In.Mode) {
  case EmitMode::EveryHit:
    return EmitDecision::Emit;

  case EmitMode::OnChange:
    // The first hit is a change from nothing having been seen yet. Without
    // that, a capture that never varies would produce no event at all and be
    // indistinguishable from a location that was never reached.
    if (!In.Previous)
      return EmitDecision::Emit;
    return *In.Previous == In.Current ? EmitDecision::Skip : EmitDecision::Emit;

  case EmitMode::FirstAndLast:
    // Every hit past the first is held rather than dropped, so the last one is
    // emitted whether or not anything changed along the way.
    return In.Previous ? EmitDecision::Hold : EmitDecision::Emit;
  }
  llvm_unreachable("unhandled EmitMode");
}

//===----------------------------------------------------------------------===//
// Expression cost control
//===----------------------------------------------------------------------===//

std::optional<std::string> lldb_private::mcp::SuggestPathForm(StringRef Expr) {
  StringRef Trimmed = Expr.trim();

  // Only a nullary call has a path form. A call with arguments is computing
  // something no member holds.
  if (!Trimmed.consume_back("()"))
    return std::nullopt;

  size_t Split = Trimmed.find_last_of(".>");
  StringRef Prefix =
      Split == StringRef::npos ? StringRef() : Trimmed.take_front(Split + 1);
  StringRef Name =
      Split == StringRef::npos ? Trimmed : Trimmed.drop_front(Split + 1);

  if (!Name.consume_front("get") || Name.empty() || !isUpper(Name[0]))
    return std::nullopt;
  if (!all_of(Name, [](char C) { return isAlnum(C) || C == '_'; }))
    return std::nullopt;

  return (Prefix + Name).str();
}

CaptureCostDecision
lldb_private::mcp::AssessCaptureCost(const CaptureCostInput &In) {
  CaptureCostDecision D;
  D.ObservedHits = In.ObservedHits;
  D.TotalHits = In.TotalHits;
  D.RemainingMs = ToMs(In.Remaining);

  // The cost already paid is the estimate of the cost still to come: a trigger
  // that has fired often enough to matter will keep firing.
  D.ProjectedMs = ToMs(In.Spent);
  if (In.ObservedHits != 0)
    D.PerHitMs = ToMs(In.Spent) / static_cast<double>(In.ObservedHits);

  // A path is a debug-info lookup and a memory read. It cannot be why a run
  // misses its deadline, so turning it off would trade data for nothing.
  if (In.Tier == ValueResolutionTier::VariablePath ||
      In.Tier == ValueResolutionTier::PersistentVariable)
    return D;

  if (In.ObservedHits == 0)
    return D;

  // Compared in integer microseconds so that the boundary does not move with a
  // rounding step.
  if (In.Spent * CaptureCostBudgetDivisor <= In.Remaining)
    return D;

  D.Disable = true;

  raw_string_ostream OS(D.Note);
  OS << formatv("stopped evaluating \"{0}\" after {1} of {2} hits: {3:F2} ms "
                "per hit projects {4:F2} ms over the {5:F2} ms left, which is "
                "more than the 1/{6} of it one capture may take.",
                In.Expr, In.ObservedHits, In.TotalHits, D.PerHitMs,
                D.ProjectedMs, D.RemainingMs, CaptureCostBudgetDivisor);
  if (std::optional<std::string> Path = SuggestPathForm(In.Expr))
    OS << formatv(" Capture \"{0}\" instead: a path is a memory read, while an "
                  "expression compiles and calls into the observed process.",
                  *Path);
  else
    OS << " A path-shaped capture is a memory read, while an expression "
          "compiles and calls into the observed process; capture the members "
          "the call would have read.";
  return D;
}

json::Value CaptureCostDecision::Render() const {
  return json::Object{
      {"observed_hits", static_cast<int64_t>(ObservedHits)},
      {"total_hits", static_cast<int64_t>(TotalHits)},
      {"per_hit_ms", Round2(PerHitMs)},
      {"projected_ms", Round2(ProjectedMs)},
      {"note", Note},
  };
}

//===----------------------------------------------------------------------===//
// Frame ranking
//===----------------------------------------------------------------------===//

namespace {

/// Roots under which a file belongs to the system or the toolchain rather than
/// to the program being observed.
constexpr StringRef SystemPathPrefixes[] = {
    "/usr/",  "/System/", "/Library/",      "/bin/",
    "/sbin/", "/opt/",    "/Applications/", "C:\\Windows\\",
};

/// Fragments that mark a toolchain path wherever it is installed.
constexpr StringRef SystemPathFragments[] = {
    "/include/c++/", "/lib/gcc/",   "/SDKs/",
    "/.rustup/",     "/Xcode.app/", "\\Program Files",
};

} // namespace

bool lldb_private::mcp::IsSystemSourcePath(StringRef File) {
  if (File.empty())
    return false;
  return any_of(SystemPathPrefixes,
                [&](StringRef P) { return File.starts_with(P); }) ||
         any_of(SystemPathFragments,
                [&](StringRef F) { return File.contains(F); });
}

std::vector<RankedFrame>
lldb_private::mcp::RankFrames(ArrayRef<RawFrame> Frames) {
  // Folding comes first and runs over unwinder order, because adjacency is what
  // makes a repeated name recursion rather than a coincidence.
  std::vector<RankedFrame> Folded;
  for (const RawFrame &Frame : Frames) {
    if (!Folded.empty() && Folded.back().Function == Frame.Function) {
      RankedFrame &Run = Folded.back();
      ++Run.Repeats;
      // A run entered through more than one call site carries the location
      // nearest the failure, which is the innermost one to resolve a file.
      if (Run.File.empty() && !Frame.File.empty()) {
        Run.File = Frame.File;
        Run.Line = Frame.Line;
        Run.IsSystem = IsSystemSourcePath(Run.File);
      }
      continue;
    }

    RankedFrame Ranked;
    Ranked.Function = Frame.Function;
    Ranked.File = Frame.File;
    Ranked.Line = Frame.Line;
    Ranked.IsSystem = IsSystemSourcePath(Frame.File);
    Folded.push_back(std::move(Ranked));
  }

  std::vector<RankedFrame> WithSource;
  for (const RankedFrame &Frame : Folded)
    if (!Frame.File.empty())
      WithSource.push_back(Frame);

  // Keeping the sourceless frames when none has source is what stops a stripped
  // binary from reporting no location at all. Where the program is remains the
  // answer even where the source does not exist.
  std::vector<RankedFrame> Kept =
      WithSource.empty() ? std::move(Folded) : std::move(WithSource);

  // Stable, so that within each group the innermost frame stays innermost:
  // among the program's own frames, order is the answer.
  llvm::stable_sort(Kept, [](const RankedFrame &A, const RankedFrame &B) {
    return static_cast<int>(A.IsSystem) < static_cast<int>(B.IsSystem);
  });
  return Kept;
}

json::Value RankedFrame::Render() const {
  json::Object O{{"function", Function}};
  if (!File.empty()) {
    O["file"] = File;
    if (Line != 0)
      O["line"] = static_cast<int64_t>(Line);
  }
  if (Repeats > 1)
    O["repeats"] = static_cast<int64_t>(Repeats);
  if (IsSystem)
    O["system"] = true;
  return O;
}

//===----------------------------------------------------------------------===//
// Result rendering
//===----------------------------------------------------------------------===//

json::Value CaptureReport::Render() const {
  // A capture that resolved as a path and never failed has nothing to say
  // beyond how it resolved, and most captures are that.
  if (!Disabled && Errors == 0 && Tier == ValueResolutionTier::VariablePath)
    return ToString(Tier);

  json::Object O{{"tier", ToString(Tier)},
                 {"evaluations", static_cast<int64_t>(Evaluations)},
                 {"total_ms", Round2(TotalMs)}};
  if (Errors != 0)
    O["errors"] = static_cast<int64_t>(Errors);
  if (Disabled)
    O["disabled"] = Disabled->Render();
  return O;
}

json::Value ObservationReport::Render() const {
  json::Object O{
      {"at", At},
      {"resolved_locations", static_cast<int64_t>(ResolvedLocations)},
      {"hits", static_cast<int64_t>(Hits)},
      {"emitted", static_cast<int64_t>(Emitted)}};
  if (ResolutionError)
    O["error"] = *ResolutionError;

  // Reported only for an observation that carried a condition, so that their
  // absence says "no condition" rather than "a condition that never held".
  if (HasCondition) {
    O["condition_true"] = static_cast<int64_t>(ConditionTrue);
    O["condition_errors"] = static_cast<int64_t>(ConditionErrors);
    O["condition_ms"] = Round2(ConditionMs);
  }

  if (!Captures.empty()) {
    json::Object Rendered;
    for (const CaptureReport &Capture : Captures)
      Rendered[Capture.Expr] = Capture.Render();
    O["captures"] = std::move(Rendered);
  }
  return O;
}

json::Value TerminalEvent::Render() const {
  json::Object O{{"description", Description}};
  if (ExitStatus)
    O["exit_status"] = static_cast<int64_t>(*ExitStatus);
  if (!Function.empty())
    O["function"] = Function;
  if (!File.empty()) {
    O["file"] = File;
    if (Line != 0)
      O["line"] = static_cast<int64_t>(Line);
  }
  if (!Frames.empty()) {
    json::Array Rendered;
    for (const RankedFrame &Frame : Frames)
      Rendered.push_back(Frame.Render());
    O["frames"] = std::move(Rendered);
    O["frames_total"] = static_cast<int64_t>(FramesTotal);
    if (FramesOmitted != 0)
      O["frames_omitted"] = static_cast<int64_t>(FramesOmitted);
  }
  if (!Locals.empty())
    O["locals"] = json::Object(Locals);
  if (!Source.empty())
    O["source"] = Source;
  return O;
}

json::Value ArtifactReport::Render() const {
  json::Object O{{"events", static_cast<int64_t>(Events)}};
  if (!Path.empty())
    O["path"] = Path;
  if (Truncated)
    O["truncated"] = true;
  if (!Tail.empty())
    O["tail"] = json::Array(Tail);

  // A path alone leaves a reader to discover the shape by opening the file.
  // Naming the fields is what makes the artifact greppable without reading it,
  // which is the point of writing it out rather than inlining it.
  if (!Path.empty()) {
    O["format"] = "one JSON object per line";
    O["fields"] = json::Array{"seq", "label", "t_ms", "frame", "values"};
  }
  return O;
}

json::Value ObservationResult::Render() const {
  json::Object O{{"outcome", ToString(Result)},
                 {"elapsed_ms", Round2(ElapsedMs)},
                 {"terminal", Terminal.Render()}};

  if (!Observations.empty()) {
    json::Object Report;
    for (const ObservationReport &Observation : Observations)
      Report[Observation.Label] = Observation.Render();
    O["plan_report"] = std::move(Report);
  }

  if (const json::Object *Agg = Aggregate.getAsObject(); Agg && !Agg->empty())
    O["aggregate"] = Aggregate;

  if (Cycle) {
    json::Array Sequence;
    for (const std::string &Label : Cycle->Sequence)
      Sequence.push_back(Label);
    O["cycle"] = json::Object{{"period", static_cast<int64_t>(Cycle->Period)},
                              {"repeats", static_cast<int64_t>(Cycle->Repeats)},
                              {"sequence", std::move(Sequence)}};
  }

  if (Artifact)
    O["artifact"] = Artifact->Render();
  if (!InferiorOutput.empty())
    O["inferior_output"] = InferiorOutput;
  if (!Notes.empty()) {
    json::Array Rendered;
    for (const std::string &Note : Notes)
      Rendered.push_back(Note);
    O["notes"] = std::move(Rendered);
  }
  return O;
}

//===----------------------------------------------------------------------===//
// Engine
//===----------------------------------------------------------------------===//

namespace lldb_private::mcp {

/// Breakpoints at the addresses frames return to.
///
/// They are re-used rather than made one-shot. A one-shot breakpoint is retired
/// by the code that runs after a stop is accepted, which a callback declining
/// the stop never reaches, so one-shot here would grow the set with every hit
/// instead of with the number of call sites.
struct ReturnSet {
  DenseMap<lldb::addr_t, lldb::BreakpointSP> Breakpoints;

  /// Frames armed and not yet seen to return. Guards against a re-used
  /// breakpoint firing for a frame nobody asked about.
  uint32_t Pending = 0;

  void SetEnabled(bool Enable) {
    for (auto &Entry : Breakpoints)
      if (Entry.second)
        Entry.second->SetEnabled(Enable);
  }
};

/// One observation's breakpoints and running counts.
struct ObservationSite {
  ObservationEngine *Engine = nullptr;
  const Observation *Obs = nullptr;
  size_t Index = 0;
  lldb::BreakpointSP Breakpoint;

  uint64_t Hits = 0;

  /// Hits that reached the emission decision, which is what \ref OnlyHit and
  /// `first_and_last` are numbered over.
  uint64_t Recorded = 0;

  uint64_t ConditionTrue = 0;
  uint64_t ConditionErrors = 0;
  uint64_t Emitted = 0;
  Micros ConditionSpent{0};

  std::optional<std::string> Previous;
  std::optional<json::Object> Held;

  struct CaptureState {
    std::string Expr;
    ValueResolutionTier Tier = ValueResolutionTier::Unresolved;
    uint64_t Evaluations = 0;
    uint64_t Errors = 0;
    Micros Spent{0};
    std::optional<CaptureCostDecision> Disabled;
  };
  std::vector<CaptureState> Captures;

  /// Where an `on: return` observation is taken.
  ReturnSet Returns;

  /// The return type of the observed function, resolved at the first entry hit
  /// so that a return observation can report the value the function produced.
  CompilerType ReturnType;
  bool ReturnTypeResolved = false;

  /// The breakpoint on the function named by `called_from`, and the addresses
  /// its frames return to.
  lldb::BreakpointSP Gate;
  ReturnSet GateReturns;
  uint32_t GateDepth = 0;

  /// Sites this one enables the first time it is hit.
  std::vector<ObservationSite *> Enables;

  /// What the breakpoint callbacks do, as members so that the engine's
  /// collection interface stays private to it.
  bool OnHit(StoppointCallbackContext *Ctx);
  bool OnReturned(StoppointCallbackContext *Ctx);
  bool OnGateEntered(StoppointCallbackContext *Ctx);
  bool OnGateLeft(StoppointCallbackContext *Ctx);
};

} // namespace lldb_private::mcp

namespace {

/// The name a return observation reports the function's own result under.
constexpr StringLiteral ReturnValueCapture = "$return";

/// Frames the terminal event reports after ranking. Deep enough to cross a
/// framework boundary, short enough that a pass-manager stack does not become
/// the response.
constexpr size_t MaxTerminalFrames = 24;

/// Locals the terminal event reports.
constexpr size_t MaxTerminalLocals = 32;

/// Source lines either side of the terminal line.
constexpr uint32_t TerminalSourceContext = 4;

/// Bytes of the program's own output that are kept. A chatty program would
/// otherwise put more into the response than the observations did.
constexpr size_t MaxInferiorOutput = 8192;

/// Hit locations kept for cycle detection. Enough for the longest period
/// DetectCycle looks for to repeat several times over.
constexpr size_t MaxTailLabels = MaxCyclePeriod * 8;

/// How long the wait loop blocks before it re-checks the run's deadline. A
/// program that is neither stopping nor hitting a tracepoint is only noticed
/// here, so the slice bounds how late a hang is reported.
constexpr Micros WaitSlice = std::chrono::milliseconds(200);

/// Separates one capture's rendering from the next in the tuple an emission
/// mode compares. A control character cannot occur inside a rendered value, so
/// two different tuples cannot join into one identical string.
constexpr char CaptureSeparator = '\x1f';

bool TracepointHit(void *Baton, StoppointCallbackContext *Ctx, lldb::user_id_t,
                   lldb::user_id_t) {
  return static_cast<ObservationSite *>(Baton)->OnHit(Ctx);
}

bool ObservationReturned(void *Baton, StoppointCallbackContext *Ctx,
                         lldb::user_id_t, lldb::user_id_t) {
  return static_cast<ObservationSite *>(Baton)->OnReturned(Ctx);
}

bool GateEntered(void *Baton, StoppointCallbackContext *Ctx, lldb::user_id_t,
                 lldb::user_id_t) {
  return static_cast<ObservationSite *>(Baton)->OnGateEntered(Ctx);
}

bool GateLeft(void *Baton, StoppointCallbackContext *Ctx, lldb::user_id_t,
              lldb::user_id_t) {
  return static_cast<ObservationSite *>(Baton)->OnGateLeft(Ctx);
}

/// The frame the callback was invoked for, or null when the stop carried none.
StackFrame *FrameOf(StoppointCallbackContext *Ctx) {
  if (!Ctx)
    return nullptr;
  ExecutionContext ExeCtx(Ctx->exe_ctx_ref);
  return ExeCtx.GetFramePtr();
}

/// The name and source location of \p Frame. The file is empty when the frame
/// resolved none.
RawFrame DescribeFrame(StackFrame &Frame) {
  RawFrame Described;
  SymbolContext SC = Frame.GetSymbolContext(lldb::eSymbolContextEverything);
  if (const char *Name = Frame.GetFunctionName())
    Described.Function = Name;
  if (SC.line_entry.IsValid()) {
    Described.File = SC.line_entry.GetFile().GetPath();
    Described.Line = SC.line_entry.line;
  }
  return Described;
}

json::Value RenderBacktrace(Thread &T, uint32_t Frames) {
  std::vector<RawFrame> Raw;
  for (uint32_t I = 0; I < Frames; ++I) {
    lldb::StackFrameSP Frame = T.GetStackFrameAtIndex(I);
    if (!Frame)
      break;
    Raw.push_back(DescribeFrame(*Frame));
  }
  json::Array Out;
  for (const RankedFrame &Frame : RankFrames(Raw))
    Out.push_back(Frame.Render());
  return Out;
}

} // namespace

bool ObservationSite::OnHit(StoppointCallbackContext *Ctx) {
  ++Hits;

  // Counted here rather than delegated to the breakpoint's ignore count. The
  // ignore count is consulted only in the asynchronous half of stop processing,
  // which a synchronous callback that declines the stop never reaches, so
  // setting it would leave the skip silently unapplied.
  if (Hits <= Obs->SkipFirst)
    return false;

  // A gate opens on a state change rather than being re-tested at each hit,
  // which is what keeps it off the per-hit path entirely. It opens at the first
  // hit that is observed and not at the first that occurs, since a skipped hit
  // is one this observation was told not to see.
  if (Hits == static_cast<uint64_t>(Obs->SkipFirst) + 1)
    for (ObservationSite *Dependent : Enables)
      if (Dependent->Breakpoint)
        Dependent->Breakpoint->SetEnabled(true);

  if (Obs->OnReturn) {
    // Resolved from the observed function's own frame. At the return address
    // this frame is gone, and the gating breakpoints share the same arming
    // path, so neither could supply the type.
    if (!ReturnTypeResolved) {
      ReturnTypeResolved = true;
      if (StackFrame *Frame = FrameOf(Ctx)) {
        SymbolContext SC =
            Frame->GetSymbolContext(lldb::eSymbolContextFunction);
        if (SC.function)
          ReturnType = SC.function->GetCompilerType().GetFunctionReturnType();
      }
    }
    Engine->ArmReturn(Returns, Ctx, ObservationReturned, this);
    return false;
  }
  return Engine->RecordHit(*this, Ctx);
}

bool ObservationSite::OnReturned(StoppointCallbackContext *Ctx) {
  // A re-used return breakpoint can be reached by a frame nobody armed it for,
  // so the count of armed frames rather than the breakpoint is what says
  // whether this return is one that was asked about.
  if (Returns.Pending == 0)
    return false;
  if (--Returns.Pending == 0)
    Returns.SetEnabled(false);
  return Engine->RecordHit(*this, Ctx);
}

bool ObservationSite::OnGateEntered(StoppointCallbackContext *Ctx) {
  if (GateDepth++ == 0 && Breakpoint)
    Breakpoint->SetEnabled(true);
  Engine->ArmReturn(GateReturns, Ctx, GateLeft, this);
  return false;
}

bool ObservationSite::OnGateLeft(StoppointCallbackContext *Ctx) {
  if (GateReturns.Pending == 0)
    return false;
  if (--GateReturns.Pending == 0)
    GateReturns.SetEnabled(false);

  // The counter, not the breakpoint, tracks depth: a recursive gating function
  // is still on the stack until the outermost of its frames returns.
  if (GateDepth != 0 && --GateDepth == 0 && Breakpoint)
    Breakpoint->SetEnabled(false);
  return false;
}

ObservationEngine::ObservationEngine(Debugger &Dbg, ObservationPlan Plan)
    : m_debugger(Dbg), m_plan(std::move(Plan)) {}

ObservationEngine::~ObservationEngine() = default;

ValueResolutionOptions ObservationEngine::CaptureOptions() const {
  ValueResolutionOptions Opts;

  // LLVM-shaped code is pointer-heavy, and routing every `I->Ops` through the
  // expression evaluator would make a hot tracepoint unusable.
  Opts.AllowPointerPaths = true;

  // A capture must not see `$3` from an earlier session, where a stale binding
  // would shadow the variable that was asked for.
  Opts.TryPersistent = false;
  Opts.SuppressPersistentResult = true;
  Opts.UseDynamic = lldb::eDynamicDontRunTarget;

  EvaluateExpressionOptions &Expr = Opts.ExprOptions;

  // Without this a capture's own execution trips the tracepoints this run
  // installed, and the observation observes itself.
  Expr.SetIgnoreBreakpoints(true);

  // Resuming other threads to break a lock, or falling back to doing so, turns
  // one capture into an unbounded amount of program execution.
  Expr.SetTryAllThreads(false);
  Expr.SetStopOthers(true);

  // A single runaway call fails its capture rather than the run.
  Expr.SetTimeout(std::chrono::milliseconds(50));
  Expr.SetUnwindOnError(true);
  Expr.SetUseDynamic(lldb::eDynamicDontRunTarget);
  Expr.SetSuppressPersistentResult(true);
  Expr.SetGenerateDebugInfo(false);
  return Opts;
}

Micros ObservationEngine::Remaining() const {
  const Micros Budget = std::chrono::seconds(m_plan.TimeoutSeconds);
  const Micros Spent =
      std::chrono::duration_cast<Micros>(Clock::now() - m_start);
  return Spent >= Budget ? Micros::zero() : Budget - Spent;
}

bool ObservationEngine::ShouldEnd(Outcome &Reason) {
  if (Remaining() == Micros::zero()) {
    Reason = Outcome::TimedOut;
    return true;
  }
  // Armed only when asked for: a plan whose triggers fire only near the end of
  // a long run is legitimate, and a stall check on by default would cut it
  // short.
  if (m_plan.NoProgressSeconds) {
    const Micros Idle =
        std::chrono::duration_cast<Micros>(Clock::now() - m_last_emit);
    if (Idle >= Micros(std::chrono::seconds(*m_plan.NoProgressSeconds))) {
      Reason = Outcome::NoProgress;
      return true;
    }
  }
  return false;
}

bool ObservationEngine::EndIfDue() {
  Outcome Reason = Outcome::TimedOut;
  if (!ShouldEnd(Reason))
    return false;
  m_requested_end = Reason;
  return true;
}

lldb::BreakpointSP ObservationEngine::ArmReturn(ReturnSet &Set,
                                                StoppointCallbackContext *Ctx,
                                                BreakpointHitCallback Cb,
                                                void *Baton) {
  StackFrame *Frame = FrameOf(Ctx);
  if (!Frame || !m_target)
    return nullptr;

  // Read out of the frame's own register context. Walking the stack at each hit
  // to ask who called is what makes the obvious implementation cost stack depth
  // times hits.
  lldb::RegisterContextSP Regs = Frame->GetRegisterContext();
  if (!Regs)
    return nullptr;
  const lldb::addr_t Return = Regs->GetReturnAddress(LLDB_INVALID_ADDRESS);
  if (Return == LLDB_INVALID_ADDRESS)
    return nullptr;

  lldb::BreakpointSP &BP = Set.Breakpoints[Return];
  if (!BP) {
    BP = m_target->CreateBreakpoint(Return, /*internal=*/true,
                                    /*request_hardware=*/false);
    if (!BP) {
      Set.Breakpoints.erase(Return);
      return nullptr;
    }
    BP->SetAutoContinue(true);
    BP->SetCallback(std::move(Cb), Baton, /*is_synchronous=*/true);
  }
  BP->SetEnabled(true);
  ++Set.Pending;
  return BP;
}

bool ObservationEngine::RecordHit(ObservationSite &Site,
                                  StoppointCallbackContext *Ctx) {
  StackFrame *Frame = FrameOf(Ctx);
  const Observation &Obs = *Site.Obs;

  if (Obs.WhenExpr) {
    const Clock::time_point Started = Clock::now();
    ValueResolution Resolved = ResolveValueDWIM(*Obs.WhenExpr, Frame, *m_target,
                                                Frame, CaptureOptions());
    Site.ConditionSpent +=
        std::chrono::duration_cast<Micros>(Clock::now() - Started);

    // A condition is never turned off by cost control. Dropping it would start
    // recording the hits it exists to exclude, which is a different run rather
    // than a cheaper one.
    std::optional<bool> Held;
    if (Resolved.Value) {
      Expected<bool> AsBool = Resolved.Value->GetValueAsBool();
      if (AsBool)
        Held = *AsBool;
      else
        consumeError(AsBool.takeError());
    }
    if (!Held) {
      ++Site.ConditionErrors;
      return EndIfDue();
    }
    if (!*Held)
      return EndIfDue();
    ++Site.ConditionTrue;
  }

  ++Site.Recorded;
  const uint64_t Seq = ++m_seq;

  json::Object Values;
  std::string Rendered;

  if (Obs.OnReturn && Site.ReturnType.IsValid() && Frame) {
    // At the return address the observed frame is already gone, so the value
    // the function produced is read from the ABI's result location rather than
    // from the frame. Captures naming the function's own locals cannot be read
    // here at all, which is what a return observation trades for the result.
    lldb::ThreadSP T = Frame->GetThread();
    Process *P = T ? T->GetProcess().get() : nullptr;
    if (P) {
      if (const lldb::ABISP &ABI = P->GetABI()) {
        if (lldb::ValueObjectSP Result = ABI->GetReturnValueObject(
                *T, Site.ReturnType, /*persistent=*/false)) {
          ValueObjectNode Node(Result);
          SerializeValueOptions SOpts;
          SOpts.MaxDepth = Obs.Depth;
          json::Value V = SerializeValue(Node, SOpts);
          std::string Text = Compact(V);
          m_aggregator.Record(Obs.Label, ReturnValueCapture, Text, Seq,
                              Site.Recorded);
          Rendered += Text;
          Rendered += CaptureSeparator;
          Values[ReturnValueCapture] = std::move(V);
        }
      }
    }
  }

  for (ObservationSite::CaptureState &Capture : Site.Captures) {
    if (Capture.Disabled)
      continue;

    // The return value is produced above, from the ABI rather than from a
    // variable of that name. Resolving it here as well would record a second
    // value under the same key on every hit, so a capture of five return values
    // would report ten, half of them unreadable.
    if (Obs.OnReturn && Capture.Expr == ReturnValueCapture)
      continue;

    const Clock::time_point Started = Clock::now();
    ValueResolution Resolved = ResolveValueDWIM(Capture.Expr, Frame, *m_target,
                                                Frame, CaptureOptions());
    const Micros Elapsed =
        std::chrono::duration_cast<Micros>(Clock::now() - Started);
    Capture.Spent += Elapsed;
    ++Capture.Evaluations;
    Capture.Tier = Resolved.Tier;
    if (Resolved.Tier == ValueResolutionTier::Unresolved)
      ++Capture.Errors;

    ValueObjectNode Node(Resolved.Value);
    SerializeValueOptions SOpts;
    SOpts.MaxDepth = Obs.Depth;
    SOpts.ArtifactRef = formatv("$artifact#seq={0}", Seq).str();
    json::Value V = SerializeValue(Node, SOpts);
    std::string Text = Compact(V);

    // A scalar serializes to {"value":"7"}, and using that document as the
    // aggregate's key would spend three quarters of the densest part of the
    // response on repeated punctuation. Unwrap the one-field case; anything
    // with structure keeps it, since there the structure is the information.
    if (const json::Object *Obj = V.getAsObject())
      if (Obj->size() == 1)
        if (std::optional<StringRef> Scalar = Obj->getString("value"))
          Text = Scalar->str();

    // The aggregate sees every recorded hit, whatever the emission mode does
    // with the event. That invariant is the whole reason reducing the stream is
    // a saving rather than a loss.
    m_aggregator.Record(Obs.Label, Capture.Expr, Text, Seq,
                        Site.Recorded);

    Rendered += Text;
    Rendered += CaptureSeparator;
    Values[Capture.Expr] = std::move(V);

    CaptureCostInput Cost;
    Cost.Tier = Capture.Tier;
    Cost.Spent = Capture.Spent;
    Cost.ObservedHits = Capture.Evaluations;
    Cost.TotalHits = Site.Hits;
    Cost.Remaining = Remaining();
    Cost.Expr = Capture.Expr;
    if (CaptureCostDecision Decision = AssessCaptureCost(Cost);
        Decision.Disable)
      Capture.Disabled = std::move(Decision);
  }

  m_tail_labels.push_back(Obs.Label);
  if (m_tail_labels.size() > MaxTailLabels)
    m_tail_labels.erase(m_tail_labels.begin());

  EmitDecisionInput Decision;
  Decision.Mode = Obs.Emit;
  Decision.Previous = Site.Previous;
  Decision.Current = Rendered;
  Decision.HitIndex = Site.Recorded;
  Decision.SkipFirst = Obs.SkipFirst;
  Decision.OnlyHit = Obs.OnlyHit;
  const EmitDecision What = DecideEmit(Decision);
  Site.Previous = std::move(Rendered);

  if (What != EmitDecision::Skip) {
    const double At =
        ToMs(std::chrono::duration_cast<Micros>(Clock::now() - m_start));
    json::Object Event{{"seq", static_cast<int64_t>(Seq)},
                       {"label", Obs.Label},
                       {"t_ms", Round2(At)}};
    if (Frame)
      Event["frame"] = DescribeFrame(*Frame).Function;
    if (!Values.empty())
      Event["values"] = std::move(Values);
    if (Obs.Backtrace != 0 && Frame)
      if (lldb::ThreadSP T = Frame->GetThread())
        Event["frames"] = RenderBacktrace(*T, Obs.Backtrace);

    if (What == EmitDecision::Emit)
      WriteEvent(Site, std::move(Event));
    else
      Site.Held = std::move(Event);
  }

  return EndIfDue();
}

void ObservationEngine::WriteEvent(ObservationSite &Site, json::Object Event) {
  ++Site.Emitted;
  m_last_emit = Clock::now();

  m_tail_events.push_back(json::Value(json::Object(Event)));
  if (m_tail_events.size() > InlinedTailEvents)
    m_tail_events.erase(m_tail_events.begin());

  if (m_artifact)
    m_artifact->Write(Event);
}

void ObservationEngine::FlushHeldEvents() {
  for (std::unique_ptr<ObservationSite> &Site : m_sites) {
    if (!Site->Held)
      continue;
    WriteEvent(*Site, std::move(*Site->Held));
    Site->Held.reset();
  }
}

Error ObservationEngine::InstallObservations() {
  std::vector<LocationResolution> Resolved =
      ResolveObservationLocations(m_plan, *m_target);

  StringMap<ObservationSite *> ByLabel;
  for (size_t I = 0; I < m_plan.Observations.size(); ++I) {
    auto Site = std::make_unique<ObservationSite>();
    Site->Engine = this;
    Site->Obs = &m_plan.Observations[I];
    Site->Index = I;
    Site->Breakpoint = Resolved[I].Breakpoint;
    for (const std::string &Expr : Site->Obs->Capture) {
      ObservationSite::CaptureState Capture;
      Capture.Expr = Expr;
      Site->Captures.push_back(std::move(Capture));
    }
    ByLabel[Site->Obs->Label] = Site.get();
    m_sites.push_back(std::move(Site));
  }

  for (std::unique_ptr<ObservationSite> &Site : m_sites) {
    const Observation &Obs = *Site->Obs;

    // The label is known to name an observation in this plan; parsing rejects a
    // gate on a label that does not exist, because it would leave the
    // observation disabled for the whole run.
    if (Obs.EnabledAfter)
      if (auto Gate = ByLabel.find(*Obs.EnabledAfter); Gate != ByLabel.end())
        Gate->second->Enables.push_back(Site.get());

    if (!Site->Breakpoint)
      continue;

    // Synchronous, because an asynchronous callback runs after the stop has
    // been broadcast, by which point declining it no longer keeps the stop from
    // surfacing. Together with auto-continue and a callback that returns false,
    // this is what makes a tracepoint a tracepoint.
    Site->Breakpoint->SetCallback(TracepointHit, Site.get(),
                                  /*is_synchronous=*/true);
    Site->Breakpoint->SetAutoContinue(true);

    // A gated observation starts disabled and is switched on by whatever opens
    // its gate, so the cost of the gate is a state change and not a stack walk
    // per hit.
    if (Obs.EnabledAfter || Obs.CalledFrom)
      Site->Breakpoint->SetEnabled(false);

    if (Obs.CalledFrom) {
      Site->Gate = m_target->CreateBreakpoint(
          /*containingModules=*/nullptr, /*containingSourceFiles=*/nullptr,
          Obs.CalledFrom->c_str(), lldb::eFunctionNameTypeAuto,
          lldb::eLanguageTypeUnknown, /*offset=*/0,
          /*offset_is_insn_count=*/false, eLazyBoolCalculate, /*internal=*/true,
          /*request_hardware=*/false);
      if (!Site->Gate || Site->Gate->GetNumLocations() == 0)
        m_result.Notes.push_back(
            formatv(
                "observation \"{0}\": \"called_from\" names \"{1}\", which "
                "matched no code, so the observation stays disabled for the "
                "whole run.",
                Obs.Label, *Obs.CalledFrom)
                .str());
      if (Site->Gate) {
        Site->Gate->SetCallback(GateEntered, Site.get(),
                                /*is_synchronous=*/true);
        Site->Gate->SetAutoContinue(true);
      }
    }
  }

  for (size_t I = 0; I < m_sites.size(); ++I) {
    ObservationReport Report;
    Report.Label = m_plan.Observations[I].Label;
    Report.At = m_plan.Observations[I].AtLine
                    ? formatv("{0}:{1}", m_plan.Observations[I].At,
                              *m_plan.Observations[I].AtLine)
                          .str()
                    : m_plan.Observations[I].At;
    Report.ResolvedLocations = Resolved[I].ResolvedLocations;
    Report.ResolutionError = Resolved[I].Error;
    Report.HasCondition = m_plan.Observations[I].WhenExpr.has_value();
    m_result.Observations.push_back(std::move(Report));
  }

  return Error::success();
}

Error ObservationEngine::Launch() {
  ProcessLaunchInfo Info;
  Info.SetExecutableFile(FileSpec(m_plan.Program),
                         /*add_exe_file_as_first_arg=*/true);
  for (const std::string &Arg : m_plan.Args)
    Info.GetArguments().AppendArgument(Arg);

  Environment Env = Host::GetEnvironment();
  for (const auto &Entry : m_plan.Env)
    Env[Entry.first()] = Entry.second;
  Info.GetEnvironment() = std::move(Env);

  if (m_plan.Cwd)
    Info.SetWorkingDirectory(FileSpec(*m_plan.Cwd));
  if (m_plan.Stdin)
    Info.AppendOpenFileAction(STDIN_FILENO, FileSpec(*m_plan.Stdin),
                              /*read=*/true, /*write=*/false);
  if (!m_plan.CaptureInferiorOutput)
    Info.GetFlags().Set(lldb::eLaunchFlagDisableSTDIO);

  // Stopping at entry is what lets the tracepoints be installed before any of
  // the program runs.
  //
  // It does not, however, bound the launch. Target::Launch waits for the first
  // stop with WaitForProcessToStop(std::nullopt, ...) unconditionally
  // (Target.cpp:3710), so a program that never reports in hangs here rather
  // than being cut off by TimeoutSeconds, whose clock only covers the run
  // itself. Bounding this needs Target::Launch to accept a timeout; until then
  // a debuggee that cannot start is the one way this tool can hang.
  Info.GetFlags().Set(lldb::eLaunchFlagStopAtEntry);

  Status Err = m_target->Launch(Info, /*stream=*/nullptr);
  if (Err.Fail())
    return createStringError(Err.AsCString("failed to launch the program"));
  if (!m_target->GetProcessSP())
    return createStringError("the program was launched but no process exists");
  return Error::success();
}

Outcome ObservationEngine::WaitForEnd() {
  Process &P = *m_target->GetProcessSP();

  // Events go to a listener of this engine's own, so that the wait is not
  // racing whatever else is draining the debugger's listener.
  lldb::ListenerSP Hijack = Listener::MakeListener("lldb.mcp.observe");
  P.HijackProcessEvents(Hijack);

  Outcome Result = Outcome::Exited;
  Status Err = P.Resume();
  if (Err.Fail()) {
    m_result.Notes.push_back(formatv("the program could not be resumed: {0}",
                                     Err.AsCString("unknown error"))
                                 .str());
    P.RestoreProcessEvents();
    return Outcome::Crashed;
  }

  while (true) {
    // The clock is read at the top of every pass, so a stop the plan did not
    // ask for cannot keep the loop going past the deadline however often it
    // recurs.
    Outcome Due = Outcome::TimedOut;
    if (ShouldEnd(Due)) {
      Result = Due;
      // State cannot be read out of a running process, and a hang gets the same
      // treatment as a crash, so it has to be stopped first.
      if (StateIsRunningState(P.GetState()))
        P.Halt(/*clear_thread_plans=*/true);
      break;
    }

    const lldb::StateType State = P.WaitForProcessToStop(
        std::min(Remaining(), WaitSlice), /*event_sp_ptr=*/nullptr,
        /*wait_always=*/true, Hijack, /*stream=*/nullptr,
        /*use_run_lock=*/true);

    DrainInferiorOutput();

    if (State == lldb::eStateExited || State == lldb::eStateDetached ||
        State == lldb::eStateUnloaded) {
      Result = Outcome::Exited;
      break;
    }

    if (State != lldb::eStateStopped && State != lldb::eStateCrashed)
      // The wait expired, which says nothing about the program beyond that it
      // is still running. The clock at the top of the loop decides what that
      // means.
      continue;

    // A callback that found the run's time was up is the only place a program
    // busy hitting tracepoints can be noticed to have overrun.
    if (m_requested_end) {
      Result = *m_requested_end;
      break;
    }

    lldb::ThreadSP T = P.GetThreadList().GetSelectedThread();
    const lldb::StopReason Reason =
        T ? T->GetStopReason() : lldb::eStopReasonNone;
    if (State == lldb::eStateCrashed || Reason == lldb::eStopReasonSignal ||
        Reason == lldb::eStopReasonException ||
        Reason == lldb::eStopReasonInstrumentation) {
      Result = Outcome::Crashed;
      break;
    }

    // Any other stop is one the plan did not ask for. Resuming is what keeps a
    // stop nobody owns from being reported as the program's end.
    if (P.Resume().Fail()) {
      Result = Outcome::Crashed;
      break;
    }
  }

  P.RestoreProcessEvents();
  return Result;
}

void ObservationEngine::DrainInferiorOutput() {
  if (!m_plan.CaptureInferiorOutput)
    return;
  lldb::ProcessSP P = m_target->GetProcessSP();
  if (!P)
    return;

  // Drained as the run goes rather than only at the end, because a full pipe
  // blocks the program that is writing to it.
  char Buffer[1024];
  for (bool Stderr : {false, true}) {
    while (true) {
      Status Err;
      const size_t Read = Stderr ? P->GetSTDERR(Buffer, sizeof(Buffer), Err)
                                 : P->GetSTDOUT(Buffer, sizeof(Buffer), Err);
      if (Read == 0)
        break;
      m_result.InferiorOutput.append(Buffer, Read);
      if (m_result.InferiorOutput.size() > MaxInferiorOutput) {
        // The tail is the part that says how far the program got, so the front
        // is what goes.
        m_result.InferiorOutput.erase(0, m_result.InferiorOutput.size() -
                                             MaxInferiorOutput);
        m_output_truncated = true;
      }
      if (Read < sizeof(Buffer))
        break;
    }
  }
}

void ObservationEngine::CollectTerminalEvent(Outcome Result) {
  TerminalEvent &Terminal = m_result.Terminal;
  lldb::ProcessSP P = m_target->GetProcessSP();

  switch (Result) {
  case Outcome::Exited:
    Terminal.Description = "the program ran to completion";
    break;
  case Outcome::Crashed:
    Terminal.Description = "the program stopped abnormally";
    break;
  case Outcome::TimedOut:
    Terminal.Description =
        formatv("the program was still running after {0}s and was stopped",
                m_plan.TimeoutSeconds)
            .str();
    break;
  case Outcome::NoProgress:
    Terminal.Description =
        formatv("no event was emitted for {0}s and the program was stopped",
                m_plan.NoProgressSeconds.value_or(0))
            .str();
    break;
  }

  if (!P)
    return;

  if (Result == Outcome::Exited) {
    Terminal.ExitStatus = P->GetExitStatus();
    if (const char *Why = P->GetExitDescription())
      Terminal.Description = Why;
    return;
  }

  lldb::ThreadSP T = P->GetThreadList().GetSelectedThread();
  if (!T)
    T = P->GetThreadList().GetThreadAtIndex(0);
  if (!T)
    return;

  if (std::string Stop = T->GetStopDescription(); !Stop.empty())
    Terminal.Description = std::move(Stop);

  std::vector<RawFrame> Raw;
  const uint32_t Depth = T->GetStackFrameCount();
  Terminal.FramesTotal = Depth;
  for (uint32_t I = 0; I < Depth; ++I) {
    lldb::StackFrameSP Frame = T->GetStackFrameAtIndex(I);
    if (!Frame)
      break;
    Raw.push_back(DescribeFrame(*Frame));
  }

  Terminal.Frames = RankFrames(Raw);
  if (Terminal.Frames.size() > MaxTerminalFrames) {
    Terminal.FramesOmitted =
        static_cast<uint32_t>(Terminal.Frames.size() - MaxTerminalFrames);
    Terminal.Frames.resize(MaxTerminalFrames);
  }
  if (!Terminal.Frames.empty()) {
    Terminal.Function = Terminal.Frames.front().Function;
    Terminal.File = Terminal.Frames.front().File;
    Terminal.Line = Terminal.Frames.front().Line;
  }

  lldb::StackFrameSP Frame = T->GetStackFrameAtIndex(0);
  if (!Frame)
    return;

  // The dynamic type is worth a call into the inferior exactly once, and this
  // is the once: a terminal event happens one time per run.
  if (VariableList *Locals = Frame->GetVariableList(
          /*get_file_globals=*/false, /*include_synthetic_vars=*/true,
          /*error_ptr=*/nullptr)) {
    const size_t Count = std::min(Locals->GetSize(), MaxTerminalLocals);
    for (size_t I = 0; I < Count; ++I) {
      lldb::VariableSP Var = Locals->GetVariableAtIndex(I);
      if (!Var)
        continue;
      lldb::ValueObjectSP Value = Frame->GetValueObjectForFrameVariable(
          Var, lldb::eDynamicCanRunTarget);
      if (!Value)
        continue;
      ValueObjectNode Node(Value);
      Terminal.Locals[Value->GetName().GetStringRef()] =
          SerializeValue(Node, SerializeValueOptions());
    }
    if (Locals->GetSize() > Count)
      Terminal.Locals["_elided"] =
          formatv("{0} more locals", Locals->GetSize() - Count).str();
  }

  SymbolContext SC = Frame->GetSymbolContext(lldb::eSymbolContextEverything);
  if (SC.line_entry.IsValid()) {
    StreamString Source;
    m_target->GetSourceManager().DisplaySourceLinesWithLineNumbers(
        SC.line_entry.file_sp, SC.line_entry.line, SC.line_entry.column,
        TerminalSourceContext, TerminalSourceContext, "->", &Source);
    Terminal.Source = Source.GetString().str();
  }
}

Expected<ObservationResult> ObservationEngine::Run() {
  m_start = Clock::now();
  m_last_emit = m_start;

  lldb::TargetSP Target;
  Status Err = m_debugger.GetTargetList().CreateTarget(
      m_debugger, m_plan.Program, /*triple_str=*/"", eLoadDependentsYes,
      /*platform_options=*/nullptr, Target);
  if (!Target)
    return createStringError(
        formatv("could not create a target for \"{0}\": {1}", m_plan.Program,
                Err.AsCString("unknown error")));
  m_target = Target;

  if (Expected<std::unique_ptr<EventArtifact>> Artifact =
          EventArtifact::Create())
    m_artifact = std::move(*Artifact);
  else
    // The artifact is where output too large to inline goes; losing it costs
    // the overflow path, not the run.
    m_result.Notes.push_back(
        formatv("events are not being recorded to a file: {0}",
                toString(Artifact.takeError()))
            .str());

  if (Error E = InstallObservations())
    return std::move(E);

  if (Error E = Launch())
    return std::move(E);

  const Outcome Result = WaitForEnd();
  FlushHeldEvents();
  DrainInferiorOutput();
  CollectTerminalEvent(Result);

  m_result.Result = Result;
  m_result.ElapsedMs =
      ToMs(std::chrono::duration_cast<Micros>(Clock::now() - m_start));
  m_result.Aggregate = m_aggregator.Render();
  if (m_output_truncated)
    m_result.Notes.push_back(
        formatv("only the last {0} bytes of the program's own output are "
                "reported; the earlier output was dropped.",
                MaxInferiorOutput)
            .str());
  // Only for a run that did not finish. A loop is how programs are written, so
  // reporting one for a program that ran to completion says a normal loop was
  // the reason it got stuck, which it was not.
  if (IsAbnormal(m_result.Result))
    m_result.Cycle = DetectCycle(m_tail_labels);

  uint64_t Emitted = 0;
  for (const std::unique_ptr<ObservationSite> &Site : m_sites) {
    Emitted += Site->Emitted;
    ObservationReport &Report = m_result.Observations[Site->Index];
    Report.Hits = Site->Hits;
    Report.ConditionTrue = Site->ConditionTrue;
    Report.ConditionErrors = Site->ConditionErrors;
    Report.ConditionMs = ToMs(Site->ConditionSpent);
    Report.Emitted = Site->Emitted;
    for (const ObservationSite::CaptureState &Capture : Site->Captures) {
      CaptureReport Rendered;
      Rendered.Expr = Capture.Expr;
      Rendered.Tier = Capture.Tier;
      Rendered.Evaluations = Capture.Evaluations;
      Rendered.Errors = Capture.Errors;
      Rendered.TotalMs = ToMs(Capture.Spent);
      Rendered.Disabled = Capture.Disabled;
      Report.Captures.push_back(std::move(Rendered));
    }

    // A capture that failed at every hit of a return observation is almost
    // always one naming state the observed function owned. Saying so is the
    // difference between a fixable mistake and an unexplained empty column.
    if (Site->Obs->OnReturn && Site->Recorded != 0 &&
        all_of(Site->Captures,
               [](const ObservationSite::CaptureState &Capture) {
                 return Capture.Evaluations != 0 &&
                        Capture.Errors == Capture.Evaluations;
               }) &&
        !Site->Captures.empty())
      m_result.Notes.push_back(
          formatv("observation \"{0}\" is taken as the function returns, where "
                  "its frame has already been popped, so nothing it owned can "
                  "still be read. The value it produced is reported as "
                  "\"{1}\"; anything else has to be captured on entry.",
                  Site->Obs->Label, ReturnValueCapture)
              .str());
  }

  // Reported even with no file behind it, so that a run whose artifact could
  // not be created still says how many events it produced and still shows the
  // last of them.
  if (m_artifact || Emitted != 0) {
    ArtifactReport Artifact;
    Artifact.Events = Emitted;
    if (m_artifact) {
      Artifact.Path = m_artifact->GetPath().str();
      Artifact.Events = m_artifact->GetEventCount();
      Artifact.Truncated = m_artifact->HitInternalLimit();
    }
    // Inlined only when the program ended badly. A run that ended badly is read
    // backwards from the end; one that ended well is read through its
    // aggregate, and the events are a file away either way.
    if (IsAbnormal(Result))
      Artifact.Tail = m_tail_events;
    m_result.Artifact = std::move(Artifact);
  }

  // The observed process must not outlive the run: it is holding a stopped
  // thread and every breakpoint this engine installed.
  if (lldb::ProcessSP P = m_target->GetProcessSP())
    if (P->IsAlive())
      P->Destroy(/*force_kill=*/true);
  m_debugger.GetTargetList().DeleteTarget(m_target);

  return std::move(m_result);
}
