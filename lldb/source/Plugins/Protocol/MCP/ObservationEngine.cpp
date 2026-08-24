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
#include "lldb/Target/StackID.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/TargetList.h"
#include "lldb/Target/Thread.h"
#include "lldb/Target/ThreadList.h"
#include "lldb/Target/UnixSignals.h"
#include "lldb/Target/StopInfo.h"
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
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallSet.h"
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

/// The share of a run that setup has to be before it is worth reporting, and
/// before the run says where the time went. Below this the tracepoints really
/// are the cost, and a second number would only repeat the first.
constexpr double SetupShareWorthReporting = 0.5;

/// How long setup has to take before it is worth prose as well as a number. A
/// tenth of a second is not a decision a caller has to make.
constexpr double SetupMsWorthANote = 2000.0;

/// Rounds to hundredths, so that a measurement does not spend twelve characters
/// asserting a precision it does not have.
double Round2(double Value) { return std::round(Value * 100.0) / 100.0; }

std::string Compact(const json::Value &V) {
  std::string S;
  raw_string_ostream OS(S);
  OS << V;
  return S;
}

/// The text a value is aggregated under. A scalar serializes to {"value":"7"},
/// and using that document as the aggregate's key would spend three quarters of
/// the densest part of the response on repeated punctuation. Anything with
/// structure keeps it, since there the structure is the information.
///
/// Every capture goes through here, including `$return`: two spellings of the
/// same number are two values to an aggregate that compares them as strings, so
/// a run's return values would be summarised separately from the paths they came
/// from and neither would be comparable with the other.
std::string AggregateKey(const json::Value &V) {
  if (const json::Object *Obj = V.getAsObject())
    if (Obj->size() == 1)
      if (std::optional<StringRef> Scalar = Obj->getString("value"))
        return Scalar->str();
  return Compact(V);
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

  // A capture that has never once produced a value is not expensive, it is
  // wrong: the name resolves nowhere the observation is taken. Left to the cost
  // test below it pays a full expression compile at every hit -- 90 ms each,
  // measured on a name that was simply out of scope -- and is then turned off
  // with advice about cheaper spelling that cannot apply, since a bare
  // identifier is already as cheap as a name gets. Eight such captures in one
  // measured run burned 30 s of a 25 s budget and returned nothing, so stopping
  // early is worth more than the chance that the name would have resolved later.
  if (In.Tier == ValueResolutionTier::Unresolved &&
      In.Errors >= UnresolvableCaptureAttempts && In.Errors == In.ObservedHits) {
    D.Disable = true;
    // A return observation has a known cause, and naming the scope instead would
    // send the caller hunting a declaration that is in the right place. Measured
    // on a live run: eight captures of a predicate's own locals, taken at its
    // return, were reported as too expensive with advice to spell them more
    // cheaply -- which a bare identifier cannot be. The remedy differs by what
    // the name is, and "on": "entry" is only right for a parameter: a local is
    // not assigned yet at entry, where it reads as uninitialised stack or as the
    // previous call's leftover value, which is plausible and wrong.
    D.Note =
        In.AtReturn
            ? formatv("stopped evaluating \"{0}\": it produced no value at any "
                      "of its {1} hits. This observation is taken as the "
                      "function returns, where its frame has already been "
                      "popped, so its parameters and locals cannot be read "
                      "there. Capture \"{2}\" for what it produced; read a "
                      "parameter with \"on\": \"entry\", and a local at a source "
                      "location after it is assigned, \"at\": "
                      "\"file.cpp:LINE\", since at entry it holds whatever was "
                      "on the stack.",
                      In.Expr, In.Errors, "$return")
                  .str()
            : formatv("stopped evaluating \"{0}\": it produced no value at any "
                      "of its {1} hits, so the name does not resolve where this "
                      "observation is taken. Check that it is in scope there -- "
                      "a local declared further down the function, or a member "
                      "of another object, resolves nowhere at this line -- "
                      "rather than spelling it more cheaply.",
                      In.Expr, In.Errors)
                  .str();
    return D;
  }

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
// Frame arming
//===----------------------------------------------------------------------===//

void ArmedFrames::Discard(SmallVectorImpl<Frame> &Frames, lldb::addr_t CFA) {
  // The stack grows down, so a frame strictly below where the thread is now has
  // been left. Strictly: a thread standing at exactly an armed frame's
  // call-frame address is standing in that frame, which is what happens the
  // moment a recursive call returns into the frame that made it.
  //
  // Armed frames run outermost-first, and an outer frame's call-frame address
  // is the higher one, so the departed frames are always a suffix.
  while (!Frames.empty() && Frames.back().CFA < CFA) {
    Frames.pop_back();
    --m_count;
    ++m_abandoned;
  }
}

void ArmedFrames::Arm(lldb::tid_t Tid, lldb::addr_t Return, lldb::addr_t CFA) {
  SmallVector<Frame, 4> &Frames = m_frames[Tid];

  // A call below an armed frame is proof that frame is gone: this thread could
  // not be running here otherwise. Dropping those now is what keeps a frame
  // lost to an exception from being reported as the return of a later call.
  Discard(Frames, CFA);

  Frames.push_back({Return, CFA});
  ++m_count;
}

bool ArmedFrames::Returned(lldb::tid_t Tid, lldb::addr_t Return,
                           lldb::addr_t CFA) {
  auto It = m_frames.find(Tid);
  if (It == m_frames.end())
    return false;
  SmallVector<Frame, 4> &Frames = It->second;

  // Only the innermost armed frame can be the one that returned here, and only
  // if the thread has left it. When it has, that frame is removed as having
  // returned rather than counted among the abandoned.
  const bool Returned = !Frames.empty() && Frames.back().CFA < CFA &&
                        Frames.back().Return == Return;
  if (Returned) {
    Frames.pop_back();
    --m_count;
  }
  Discard(Frames, CFA);

  if (Frames.empty())
    m_frames.erase(It);
  return Returned;
}

bool ArmedFrames::EnclosesFrame(lldb::tid_t Tid, lldb::addr_t CFA) {
  auto It = m_frames.find(Tid);
  if (It == m_frames.end())
    return false;

  Discard(It->second, CFA);
  if (It->second.empty()) {
    m_frames.erase(It);
    return false;
  }
  // Strictly outer, so that a function observed as being called from itself
  // counts its nested calls and not the outermost one, which nothing called.
  return It->second.back().CFA > CFA;
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
// Stack profile
//===----------------------------------------------------------------------===//

void StackProfile::Record(lldb::tid_t Tid, ArrayRef<RawFrame> InnermostFirst) {
  if (InnermostFirst.empty())
    return;

  ++m_samples;
  ++m_per_thread[Tid];

  const RawFrame &Innermost = InnermostFirst.front();
  Site &S = m_sites[{Tid, Innermost.Function}];
  const bool First = S.Samples == 0;
  if (First) {
    S.Function = Innermost.Function;
    S.File = Innermost.File;
    S.Line = Innermost.Line;
    S.Tid = Tid;
    S.FirstSample = m_samples;
  }
  ++S.Samples;

  // The callers, outermost first, so that intersecting is taking a common
  // prefix: the frames a stack shares with another are the ones nearest the
  // bottom, since two stacks agree about how the program got here and disagree
  // about where here is. The innermost frame is left out because it is the site
  // itself, which `hot` names -- and because after an intersection it may not be
  // there to recognise, two paths into one function sharing only `main`.
  std::vector<std::string> Outermost;
  Outermost.reserve(InnermostFirst.size());
  for (const RawFrame &Frame : reverse(InnermostFirst.drop_front()))
    Outermost.push_back(Frame.Function);

  if (First) {
    S.Shared = std::move(Outermost);
    return;
  }
  size_t Common = 0;
  while (Common < S.Shared.size() && Common < Outermost.size() &&
         S.Shared[Common] == Outermost[Common])
    ++Common;
  S.Shared.resize(Common);
}

json::Value StackProfile::Render() const {
  if (m_samples == 0)
    return nullptr;

  const bool Threaded = m_per_thread.size() > 1;

  std::vector<const Site *> Ranked;
  Ranked.reserve(m_sites.size());
  for (const auto &[Key, S] : m_sites)
    Ranked.push_back(&S);

  // A thread parked in a wait is sampled as often as one burning a core, and its
  // innermost frame is the same one every time while a working thread's varies,
  // so ranking on samples alone puts the idle thread first: measured on a program
  // with one spinning thread and one asleep, 19 samples in `__semwait_signal`
  // against 11 in the hot function. Whether the frame resolved to source is what
  // separates them, and it is the same partition a backtrace is ranked by, for
  // the same reason: a frame whose definition the caller cannot see is not where
  // the caller's problem is.
  llvm::stable_sort(Ranked, [](const Site *LHS, const Site *RHS) {
    const bool LSys = LHS->File.empty();
    const bool RSys = RHS->File.empty();
    if (LSys != RSys)
      return RSys;
    return std::tie(RHS->Samples, LHS->FirstSample) <
           std::tie(LHS->Samples, RHS->FirstSample);
  });

  json::Array Hot;
  const size_t Kept = std::min<size_t>(Ranked.size(), MaxHot);
  for (size_t I = 0; I < Kept; ++I) {
    const Site &S = *Ranked[I];
    json::Object Entry{{"function", S.Function}, {"samples", S.Samples}};
    if (!S.File.empty()) {
      Entry["file"] = S.File;
      if (S.Line != 0)
        Entry["line"] = static_cast<int64_t>(S.Line);
    }
    if (Threaded)
      Entry["tid"] = static_cast<int64_t>(S.Tid);
    Hot.push_back(std::move(Entry));
  }

  json::Object O{{"samples", m_samples}, {"hot", std::move(Hot)}};
  if (Kept < Ranked.size())
    O["hot_elided"] = static_cast<int64_t>(Ranked.size() - Kept);
  if (Threaded)
    O["threads"] = static_cast<int64_t>(m_per_thread.size());

  // How the program reached the place it was found in most often. One path, for
  // the first entry above, because the entries under it are mostly its callers
  // and callees and would repeat most of it.
  ArrayRef<std::string> Shared(Ranked.front()->Shared);
  // Innermost first, matching the order a backtrace is reported in, and keeping
  // that end when the bound cuts.
  Shared = Shared.take_back(std::min<size_t>(Shared.size(), MaxUnder));
  json::Array Under;
  for (const std::string &Function : reverse(Shared))
    Under.push_back(Function);
  if (!Under.empty())
    O["under"] = std::move(Under);
  return O;
}

//===----------------------------------------------------------------------===//
// Result rendering
//===----------------------------------------------------------------------===//

json::Value CaptureReport::Render() const {
  // `$return` is read out of the ABI's result location rather than resolved
  // from a name, so it has no tier and costs no evaluation. Reporting it as an
  // expression that was never evaluated would mark the one capture that cannot
  // fail as the one that did.
  if (FromABI)
    return "abi";

  // A capture that resolved as a path and never failed has nothing to say
  // beyond how it resolved, and most captures are that.
  if (!Disabled && Errors == 0 && Tier == ValueResolutionTier::VariablePath)
    return ToString(Tier);

  // Never evaluated is not the same as could not be read: the observation may
  // never have been hit, or its condition never held. The default tier renders
  // "unavailable", which is the word an expression that genuinely failed gets,
  // so reporting it here would collapse the distinction the rest of plan_report
  // is built to keep. The counts stay beside it either way -- `evaluations: 0`
  // is what says which of the two this is.
  const std::string TierName = Evaluations == 0 && !Disabled
                                   ? std::string("not_evaluated")
                                   : ToString(Tier).str();

  json::Object O{{"tier", TierName},
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

  // One thread is the case a reader assumes, so saying so would be noise. More
  // than one changes how every other number here reads, so it is not.
  if (Threads > 1)
    O["threads"] = static_cast<int64_t>(Threads);
  if (ReturnsAbandoned != 0)
    O["returns_abandoned"] = static_cast<int64_t>(ReturnsAbandoned);

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
  // which is the point of writing it out rather than inlining it. Every name
  // here has to be one the lines actually carry, so `frames` appears only for a
  // plan that asked for a backtrace.
  if (!Path.empty()) {
    O["format"] = "one JSON object per line";
    json::Array Fields{"seq", "label", "tid", "t_ms", "frame"};
    if (CarriesBacktrace)
      Fields.push_back("frames");
    Fields.push_back("values");
    O["fields"] = std::move(Fields);
  }
  return O;
}

json::Value ObservationResult::Render() const {
  json::Object O{{"outcome", ToString(Result)},
                 {"elapsed_ms", Round2(ElapsedMs)},
                 {"terminal", Terminal.Render()}};

  // Reported only when it is most of the run, which is when a caller would
  // otherwise attribute it to the tracepoints and stop using them. Below that it
  // is a second number saying what the first one already said.
  if (SetupMs >= SetupShareWorthReporting * ElapsedMs)
    O["setup_ms"] = Round2(SetupMs);

  if (!Observations.empty()) {
    json::Object Report;
    for (const ObservationReport &Observation : Observations)
      Report[Observation.Label] = Observation.Render();
    O["plan_report"] = std::move(Report);
  }

  if (const json::Object *Agg = Aggregate.getAsObject(); Agg && !Agg->empty())
    O["aggregate"] = Aggregate;

  if (!Profile.getAsNull())
    O["profile"] = Profile;

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

/// Breakpoints at the addresses frames return to, and the frames armed at them.
///
/// The breakpoints are re-used rather than made one-shot. A one-shot breakpoint
/// is retired by the code that runs after a stop is accepted, which a callback
/// declining the stop never reaches, so one-shot here would grow the set with
/// every hit instead of with the number of call sites. Which frames are
/// outstanding is therefore tracked separately, in \ref Armed, since a shared
/// breakpoint cannot answer that itself.
struct ReturnSet {
  DenseMap<lldb::addr_t, lldb::BreakpointSP> Breakpoints;
  ArmedFrames Armed;

  /// Frames that could not be armed at all, because the return address or the
  /// call-frame address could not be read. Counted rather than ignored: an
  /// observation that recorded nothing because of this is otherwise
  /// indistinguishable from one whose code never ran.
  uint64_t ArmFailures = 0;

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

  /// Hits that reached the emission decision. Both `only_hit` and the hit an
  /// outlier names are this plus `skip_first`, so that the number the aggregate
  /// reports is the number a plan can ask for.
  uint64_t Recorded = 0;

  uint64_t ConditionTrue = 0;
  uint64_t ConditionErrors = 0;
  uint64_t Emitted = 0;
  Micros ConditionSpent{0};

  /// Threads this observation was hit on. A DenseSet is not usable here:
  /// DenseMapInfo<uint64_t> reserves ~0ULL, which is a thread id like any
  /// other.
  SmallSet<lldb::tid_t, 4> Threads;

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

  /// The breakpoint on the function named by `called_from`, and the frames of
  /// it that are currently on a stack.
  lldb::BreakpointSP Gate;
  ReturnSet GateReturns;

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

/// What a capture that ran and produced no value reads as. Spelled the way a
/// type is rather than the way a value is, since no value of a program's own can
/// render as this and be confused with it.
constexpr StringLiteral VoidValue = "(void)";

/// Frames the terminal event reports after ranking. Deep enough to cross a
/// framework boundary, short enough that a pass-manager stack does not become
/// the response.
constexpr size_t MaxTerminalFrames = 24;

/// Locals the terminal event reports.
constexpr size_t MaxTerminalLocals = 32;

/// Value-tree nodes the terminal event's locals may spend between them. Sized so
/// that a frame of scalars and small structs comes back whole, while one local
/// that reaches an arbitrarily large object cannot crowd out the rest.
constexpr unsigned MaxTerminalLocalNodes = 96;

/// The reciprocal of the share of a run that may be spent stopping the program
/// to sample it. Ten per cent: enough that a run long enough to be worth
/// profiling gets tens of samples, and little enough that a program which needed
/// most of its ceiling is not reported as having hung because of the profiling.
constexpr int64_t SampleCostBudgetDivisor = 10;

/// Threads sampled per sample, and frames walked per thread. Both are paid at
/// every sample of a run that may last minutes, and a program with hundreds of
/// threads is one where the profile is a shape rather than a list.
constexpr uint32_t MaxSampledThreads = 8;
constexpr uint32_t MaxSampledFrames = 64;

/// Whether a stop looks like the debugger interrupting the program rather than
/// the program failing. A halt arrives as a SIGSTOP, so the signal has to be
/// identified rather than the kind of stop: every other way a program stops on a
/// signal is a way it went wrong.
bool IsInterruption(Process &P, Thread *T, lldb::StopReason Reason) {
  if (Reason == lldb::eStopReasonNone)
    return true;
  if (Reason != lldb::eStopReasonSignal || !T)
    return false;
  lldb::StopInfoSP Info = T->GetStopInfo();
  if (!Info)
    return false;
  const lldb::UnixSignalsSP Signals = P.GetUnixSignals();
  return Signals && static_cast<int32_t>(Info->GetValue()) ==
                        Signals->GetSignalNumberFromName("SIGSTOP");
}

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

/// What identifies one frame of one thread for the whole time it is on the
/// stack. A return address does not: two threads share it, and a frame can be
/// left without ever reaching it.
struct FrameIdentity {
  lldb::tid_t Tid = LLDB_INVALID_THREAD_ID;

  /// The frame's call-frame address, which orders it against every other frame
  /// on its thread.
  lldb::addr_t CFA = LLDB_INVALID_ADDRESS;

  /// Where the thread is stopped.
  lldb::addr_t PC = LLDB_INVALID_ADDRESS;
};

FrameIdentity IdentifyFrame(StackFrame *Frame, Target &Tgt) {
  FrameIdentity Id;
  if (!Frame)
    return Id;
  if (lldb::ThreadSP T = Frame->GetThread())
    Id.Tid = T->GetID();
  // Without the metadata, because these are compared as numbers to order frames
  // on a stack, and the metadata form can carry pointer-authentication bits
  // that do not survive that comparison.
  Id.CFA = Frame->GetStackID().GetCallFrameAddressWithoutMetadata();
  Id.PC = Frame->GetFrameCodeAddress().GetLoadAddress(&Tgt);
  return Id;
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
  StackFrame *Frame = FrameOf(Ctx);
  const FrameIdentity Id = IdentifyFrame(Frame, *Engine->m_target);

  // A gate is answered from the stack rather than remembered from the moment it
  // was entered: being called from a function is having one of its frames still
  // below this one, on this thread. Remembering it instead would open the gate
  // for every other thread as well.
  //
  // Tested before the hit is counted, because a hit reached some other way is
  // not this observation's hit at all -- `called_from` reduces hits, not just
  // events.
  if (Obs->CalledFrom && !GateReturns.Armed.EnclosesFrame(Id.Tid, Id.CFA))
    return false;

  ++Hits;
  Threads.insert(Id.Tid);

  // Any arrival at a tracepoint is the plan seeing the program move, whatever
  // the emission mode goes on to do with the event.
  Engine->NoteProgress();

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
      if (Frame) {
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
  StackFrame *Frame = FrameOf(Ctx);
  const FrameIdentity Id = IdentifyFrame(Frame, *Engine->m_target);

  // A re-used return breakpoint is reached by frames nobody armed it for: the
  // same call site calling again, or another thread passing through. Which
  // frame this is, rather than which address, is what says whether the return
  // is one that was asked about.
  const bool Returned = Returns.Armed.Returned(Id.Tid, Id.PC, Id.CFA);
  if (Returns.Armed.Empty())
    Returns.SetEnabled(false);
  if (!Returned)
    return false;

  Engine->NoteProgress();
  return Engine->RecordHit(*this, Ctx);
}

bool ObservationSite::OnGateEntered(StoppointCallbackContext *Ctx) {
  Engine->ArmReturn(GateReturns, Ctx, GateLeft, this);

  // Enabling the tracepoint is an optimization on top of the per-hit test in
  // OnHit: it keeps the callback from running at all while no thread is inside
  // the gating function.
  if (Breakpoint && !GateReturns.Armed.Empty())
    Breakpoint->SetEnabled(true);
  return false;
}

bool ObservationSite::OnGateLeft(StoppointCallbackContext *Ctx) {
  StackFrame *Frame = FrameOf(Ctx);
  const FrameIdentity Id = IdentifyFrame(Frame, *Engine->m_target);
  GateReturns.Armed.Returned(Id.Tid, Id.PC, Id.CFA);

  // The frames, not the breakpoint, track who is inside: a recursive gating
  // function is still on the stack until the outermost of its frames returns,
  // and another thread may be inside it either way.
  if (GateReturns.Armed.Empty()) {
    GateReturns.SetEnabled(false);
    if (Breakpoint)
      Breakpoint->SetEnabled(false);
  }
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
        std::chrono::duration_cast<Micros>(Clock::now() - m_last_progress);
    if (Idle >= Micros(std::chrono::seconds(*m_plan.NoProgressSeconds))) {
      Reason = Outcome::NoProgress;
      return true;
    }
  }
  return false;
}

void ObservationEngine::NoteProgress() { m_last_progress = Clock::now(); }

bool ObservationEngine::EndIfDue() {
  Outcome Reason = Outcome::TimedOut;
  if (!ShouldEnd(Reason))
    return false;
  m_requested_end = Reason;
  return true;
}

void ObservationEngine::ArmReturn(ReturnSet &Set, StoppointCallbackContext *Ctx,
                                  BreakpointHitCallback Cb, void *Baton) {
  StackFrame *Frame = FrameOf(Ctx);
  if (!Frame || !m_target) {
    ++Set.ArmFailures;
    return;
  }

  // Read out of the frame's own register context. Walking the stack at each hit
  // to ask who called is what makes the obvious implementation cost stack depth
  // times hits.
  lldb::RegisterContextSP Regs = Frame->GetRegisterContext();
  if (!Regs) {
    ++Set.ArmFailures;
    return;
  }
  const lldb::addr_t Return = Regs->GetReturnAddress(LLDB_INVALID_ADDRESS);
  if (Return == LLDB_INVALID_ADDRESS) {
    ++Set.ArmFailures;
    return;
  }

  const FrameIdentity Id = IdentifyFrame(Frame, *m_target);
  if (Id.CFA == LLDB_INVALID_ADDRESS) {
    // Without a call-frame address there is nothing to tell this frame from the
    // next one at the same address, and a return reported for the wrong frame
    // is worse than one not reported at all.
    ++Set.ArmFailures;
    return;
  }

  lldb::BreakpointSP &BP = Set.Breakpoints[Return];
  if (!BP) {
    BP = m_target->CreateBreakpoint(Return, /*internal=*/true,
                                    /*request_hardware=*/false);
    if (!BP) {
      Set.Breakpoints.erase(Return);
      ++Set.ArmFailures;
      return;
    }
    BP->SetAutoContinue(true);
    BP->SetCallback(std::move(Cb), Baton, /*is_synchronous=*/true);
  }
  BP->SetEnabled(true);
  Set.Armed.Arm(Id.Tid, Return, Id.CFA);
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

  // Numbered over all of this observation's hits rather than over the ones that
  // survived skip_first, because that is the numbering `only_hit` takes. An
  // aggregate that named its hits the other way would send a caller acting on
  // one of them to a hit it never meant.
  const uint64_t Hit = static_cast<uint64_t>(Obs.SkipFirst) + Site.Recorded;

  // `only_hit` selects one hit, and reading state at the others buys nothing:
  // the event is going to be skipped, and the aggregate a caller would have got
  // from them is what the run that named this hit already reported. Not skipping
  // makes the documented follow-up loop -- re-run for the one interesting hit,
  // where "cost does not matter at one hit, so captures can be as generous as
  // you like" -- pay every capture at every hit instead: a 25 ms expression on a
  // tracepoint hit four thousand times costs 100 s rather than 25 ms, and a
  // capture that prints, which is how a compiler dumps a node, prints four
  // thousand times over.
  const bool ReadState = !Obs.OnlyHit || Hit == *Obs.OnlyHit;

  json::Object Values;
  std::string Rendered;

  if (ReadState && Obs.OnReturn && Site.ReturnType.IsValid() && Frame) {
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
          std::string Text = AggregateKey(V);
          m_aggregator.Record(Obs.Label, ReturnValueCapture, Text, Seq, Hit);
          Rendered += Text;
          Rendered += CaptureSeparator;
          Values[ReturnValueCapture] = std::move(V);
        }
      }
    }
  }

  for (ObservationSite::CaptureState &Capture : Site.Captures) {
    if (!ReadState)
      break;
    if (Capture.Disabled)
      continue;

    // The return value is produced above, from the ABI rather than from a
    // variable of that name. Resolving it here as well would record a second
    // value under the same key on every hit, so a capture of five return values
    // would report ten, half of them unreadable.
    if (Obs.OnReturn && Capture.Expr == ReturnValueCapture)
      continue;

    // At a return site frame #0 is the *caller*: the observed frame was popped
    // when the function returned. Resolving against it does not merely fail to
    // read what the function owned -- for a recursive function, or any caller
    // holding a name the callee also used, it succeeds and reports the caller's
    // value under the callee's name. Measured on a five-deep recursion, a
    // capture of the parameter came back shifted by exactly one frame, which in
    // an aggregate is indistinguishable from the right answer. A missing value
    // announces itself and a wrong one does not, so resolution is scoped to the
    // thread: globals and memory reads keep working, and there is no frame for a
    // local to be found in.
    ExecutionContextScope *Scope = Frame;
    if (Obs.OnReturn) {
      lldb::ThreadSP T = Frame ? Frame->GetThread() : nullptr;
      Scope = T ? static_cast<ExecutionContextScope *>(T.get())
                : static_cast<ExecutionContextScope *>(m_target.get());
    }

    const Clock::time_point Started = Clock::now();
    ValueResolution Resolved =
        ResolveValueDWIM(Capture.Expr, Obs.OnReturn ? nullptr : Frame,
                         *m_target, Scope, CaptureOptions());
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

    // A call returning void completes and leaves a value object with no type
    // carrying a placeholder error, which serializes as a value that could not
    // be read. Reporting a call that ran as a failure costs more than saying
    // nothing: the caller re-spells a capture that was working, and stops
    // looking in `inferior_output` for what it printed -- which is the whole
    // point of capturing a dump.
    if (Resolved.Tier == ValueResolutionTier::Expression && Resolved.Value &&
        !Resolved.Value->GetCompilerType().IsValid())
      V = json::Object{{"value", VoidValue}};

    std::string Text = AggregateKey(V);

    // The aggregate sees every recorded hit, whatever the emission mode does
    // with the event. That invariant is the whole reason reducing the stream is
    // a saving rather than a loss.
    m_aggregator.Record(Obs.Label, Capture.Expr, Text, Seq, Hit);

    Rendered += Text;
    Rendered += CaptureSeparator;
    Values[Capture.Expr] = std::move(V);

    CaptureCostInput Cost;
    Cost.Tier = Capture.Tier;
    Cost.Spent = Capture.Spent;
    Cost.ObservedHits = Capture.Evaluations;
    Cost.TotalHits = Site.Hits;
    Cost.Remaining = Remaining();
    Cost.Errors = Capture.Errors;
    Cost.AtReturn = Obs.OnReturn;
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

    // Hits of one observation are numbered and compared as a single sequence,
    // so on a program with more than one thread that sequence is an
    // interleaving. The thread is what lets a reader take it apart again.
    if (lldb::ThreadSP T = Frame ? Frame->GetThread() : lldb::ThreadSP())
      Event["tid"] = static_cast<int64_t>(T->GetID());
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

    if (State != lldb::eStateStopped && State != lldb::eStateCrashed) {
      // The wait expired, which says nothing about the program beyond that it
      // is still running. The clock at the top of the loop decides what that
      // means.
      //
      // It also means the program is running free rather than busy hitting
      // tracepoints, which is when a sample is both cheap and worth taking.
      // Asking for the stop here and reading it on the next pass is what keeps
      // the sampling out of the stop handling: the halt arrives as a stop
      // nothing in the plan owns, which the loop already knows how to resume
      // from.
      if (StateIsRunningState(P.GetState()) && SamplingIsAffordable()) {
        m_halt_for_sample = true;
        m_sample_started = Clock::now();
        P.Halt(/*clear_thread_plans=*/false);
      }
      continue;
    }

    // A callback that found the run's time was up is the only place a program
    // busy hitting tracepoints can be noticed to have overrun.
    if (m_requested_end) {
      Result = *m_requested_end;
      break;
    }

    lldb::ThreadSP T = P.GetThreadList().GetSelectedThread();
    const lldb::StopReason Reason =
        T ? T->GetStopReason() : lldb::eStopReasonNone;
    // Claimed only for the stop that follows the halt, and only if it looks like
    // an interruption: a program that really died on a signal in the window
    // between asking and stopping still reports as having died.
    const bool OurHalt = std::exchange(m_halt_for_sample, false) &&
                         IsInterruption(P, T.get(), Reason);
    if (!OurHalt &&
        (State == lldb::eStateCrashed || Reason == lldb::eStopReasonSignal ||
         Reason == lldb::eStopReasonException ||
         Reason == lldb::eStopReasonInstrumentation)) {
      Result = Outcome::Crashed;
      break;
    }

    // Any other stop is one the plan did not ask for. Resuming is what keeps a
    // stop nobody owns from being reported as the program's end.
    //
    // It is also where a sample is taken: a stop nobody owns is either the halt
    // the pass above asked for or something equally incidental, and in both cases
    // the stack is what the program was doing when it was interrupted.
    SampleStacks(P);
    const Status Resumed = P.Resume();
    if (OurHalt)
      m_sample_cost +=
          std::chrono::duration_cast<Micros>(Clock::now() - m_sample_started);
    if (Resumed.Fail()) {
      Result = Outcome::Crashed;
      break;
    }
  }

  P.RestoreProcessEvents();
  return Result;
}

bool ObservationEngine::SamplingIsAffordable() const {
  const Micros Elapsed =
      std::chrono::duration_cast<Micros>(Clock::now() - m_start);
  return m_sample_cost * SampleCostBudgetDivisor <= Elapsed;
}

void ObservationEngine::SampleStacks(Process &P) {
  ThreadList &Threads = P.GetThreadList();
  const uint32_t Count = std::min<uint32_t>(
      static_cast<uint32_t>(Threads.GetSize()), MaxSampledThreads);
  for (uint32_t I = 0; I < Count; ++I) {
    lldb::ThreadSP T = Threads.GetThreadAtIndex(I);
    if (!T)
      continue;

    // Bounded because the walk is paid at every sample, and because the frames
    // past this depth are the ones a shared path would have elided anyway.
    std::vector<RawFrame> Frames;
    const uint32_t Depth =
        std::min<uint32_t>(T->GetStackFrameCount(), MaxSampledFrames);
    for (uint32_t F = 0; F < Depth; ++F) {
      lldb::StackFrameSP Frame = T->GetStackFrameAtIndex(F);
      if (!Frame)
        break;
      Frames.push_back(DescribeFrame(*Frame));
    }
    m_profile.Record(T->GetID(), Frames);
  }
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
    // "Hit", not "emitted": progress is any arrival at a tracepoint, so a run
    // whose condition never holds keeps making progress and never ends here.
    // Saying "emitted" would send a reader to change an emission mode that had
    // nothing to do with it.
    Terminal.Description =
        formatv("no tracepoint was hit for {0}s and the program was stopped",
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

  // The thread's own account of why it stopped is the better description of a
  // crash, which is where it says something like EXC_BAD_ACCESS. It is the
  // worse one for a run this engine stopped itself: there the stop reason
  // describes the halt that was just delivered, so it would replace "still
  // running after 10s" with "signal SIGSTOP" and leave a reader to work out
  // that the signal was ours.
  if (Result == Outcome::Crashed)
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
    // One budget for the whole set rather than one per local. A frame holding a
    // reference to a compiler's pass manager reaches everything the compiler
    // owns in two hops, and a per-local budget lets each of forty locals spend
    // it: measured at 8.6 kB of interior, against 5 kB for the backtrace it was
    // meant to annotate. Exhausting it leaves the remaining locals as elision
    // markers, which is what the reader wants of a local it has not asked about.
    unsigned Budget = MaxTerminalLocalNodes;
    size_t Rendered = 0;
    for (size_t I = 0; I < Count; ++I) {
      lldb::VariableSP Var = Locals->GetVariableAtIndex(I);
      if (!Var)
        continue;
      lldb::ValueObjectSP Value = Frame->GetValueObjectForFrameVariable(
          Var, lldb::eDynamicCanRunTarget);
      if (!Value)
        continue;
      // A local with no name cannot be named in a capture either, so reporting
      // it costs a reader an entry keyed on the empty string and offers nothing
      // to do with it.
      llvm::StringRef Name = Value->GetName().GetStringRef();
      if (Name.empty())
        continue;
      ValueObjectNode Node(Value);
      SerializeValueOptions SOpts;
      SOpts.SharedBudget = &Budget;
      Terminal.Locals[Name] = SerializeValue(Node, SOpts);
      ++Rendered;
    }
    if (Locals->GetSize() > Rendered)
      Terminal.Locals["_elided"] =
          formatv("{0} more locals", Locals->GetSize() - Rendered).str();
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

void ObservationEngine::Teardown() {
  if (!m_target)
    return;

  // The observed process must not outlive the run: it is holding a stopped
  // thread and every breakpoint this engine installed.
  if (lldb::ProcessSP P = m_target->GetProcessSP())
    if (P->IsAlive())
      P->Destroy(/*force_kill=*/true);
  m_debugger.GetTargetList().DeleteTarget(m_target);
  m_target.reset();
}

Expected<ObservationResult> ObservationEngine::Run() {
  m_start = Clock::now();
  m_last_progress = m_start;

  lldb::TargetSP Target;
  Status Err = m_debugger.GetTargetList().CreateTarget(
      m_debugger, m_plan.Program, /*triple_str=*/"", eLoadDependentsYes,
      /*platform_options=*/nullptr, Target);
  if (!Target)
    return createStringError(
        formatv("could not create a target for \"{0}\": {1}", m_plan.Program,
                Err.AsCString("unknown error")));
  m_target = Target;

  // Every path out of here from this point on has a target to drop, including
  // the ones that fail before the program runs. A debugger this run does not
  // own would otherwise accumulate one target per failed call, each holding
  // this run's breakpoints.
  llvm::scope_exit Cleanup([this] { Teardown(); });

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

  m_result.SetupMs =
      ToMs(std::chrono::duration_cast<Micros>(Clock::now() - m_start));

  const Outcome Result = WaitForEnd();
  FlushHeldEvents();
  DrainInferiorOutput();
  CollectTerminalEvent(Result);

  m_result.Result = Result;
  m_result.ElapsedMs =
      ToMs(std::chrono::duration_cast<Micros>(Clock::now() - m_start));
  m_result.Aggregate = m_aggregator.Render();
  m_result.Profile = m_profile.Render();
  if (m_output_truncated)
    m_result.Notes.push_back(
        formatv("only the last {0} bytes of the program's own output are "
                "reported; the earlier output was dropped.",
                MaxInferiorOutput)
            .str());
  // Said in words as well as in a number, because the wrong reading of the
  // number is expensive and self-confirming: a caller that attributes a fixed
  // per-image cost to the tracepoints concludes that observing is orders of
  // magnitude too slow to measure with, and stops -- which is what happened.
  if (m_result.SetupMs >= SetupMsWorthANote &&
      m_result.SetupMs >= SetupShareWorthReporting * m_result.ElapsedMs)
    m_result.Notes.push_back(
        formatv("{0} of the {1} ms went on reading this program's debug info "
                "and resolving the tracepoints, before it started running. That "
                "is charged once per binary rather than per hit, and this "
                "session now has it: another run of the same binary is that "
                "much cheaper, and rebuilding it pays again.",
                Round2(m_result.SetupMs), Round2(m_result.ElapsedMs))
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
    Report.Threads = static_cast<uint32_t>(Site->Threads.size());

    // Once the program has exited, a frame still armed is one that never
    // returned; while it was running it might only not have returned yet, which
    // is a different thing and not counted as a loss.
    Report.ReturnsAbandoned = Site->Returns.Armed.GetAbandoned();
    if (Result == Outcome::Exited)
      Report.ReturnsAbandoned += Site->Returns.Armed.GetOutstanding();

    // Re-read now rather than trusting what the name resolved to before launch.
    // A name in a library that is loaded during the run resolves when it loads,
    // and a report still saying it matched nothing -- next to a hit count that
    // says it did -- gives the one answer this field exists to rule out.
    if (Site->Breakpoint) {
      Report.ResolvedLocations =
          static_cast<uint32_t>(Site->Breakpoint->GetNumLocations());
      if (Report.ResolvedLocations != 0)
        Report.ResolutionError.reset();
    }

    for (const ObservationSite::CaptureState &Capture : Site->Captures) {
      CaptureReport Rendered;
      Rendered.Expr = Capture.Expr;
      Rendered.Tier = Capture.Tier;
      Rendered.Evaluations = Capture.Evaluations;
      Rendered.Errors = Capture.Errors;
      Rendered.TotalMs = ToMs(Capture.Spent);
      Rendered.Disabled = Capture.Disabled;
      Rendered.FromABI =
          Site->Obs->OnReturn && Capture.Expr == ReturnValueCapture;
      Report.Captures.push_back(std::move(Rendered));
    }

    // A capture that failed at every hit of a return observation is almost
    // always one naming state the observed function owned. Saying so is the
    // difference between a fixable mistake and an unexplained empty column.
    // Any one such capture earns the note rather than only a plan where every
    // capture failed: one global that resolved alongside three locals that could
    // not does not make the three self-explanatory, and naming `$return`
    // explicitly must not buy silence either, since it never evaluates.
    if (Site->Obs->OnReturn && Site->Recorded != 0 &&
        any_of(Site->Captures,
               [](const ObservationSite::CaptureState &Capture) {
                 return Capture.Expr != ReturnValueCapture &&
                        Capture.Evaluations != 0 &&
                        Capture.Errors == Capture.Evaluations;
               }))
      m_result.Notes.push_back(
          formatv("observation \"{0}\" is taken as the function returns, where "
                  "its frame has already been popped, so nothing it owned can "
                  "still be read -- and the caller's frame, which is what is "
                  "current there, is deliberately not read in its place. The "
                  "value it produced is reported as \"{1}\" and state outside "
                  "the frame such as a global still reads. Read a parameter "
                  "with \"on\": \"entry\"; read a local at a source location "
                  "past its assignment, since at entry it has not been assigned "
                  "and holds whatever the stack held.",
                  Site->Obs->Label, ReturnValueCapture)
              .str());

    // A frame that never returns is how an exception or a longjmp leaves one,
    // and it is the reason a return observation can report fewer hits than the
    // entry it is derived from. Unexplained, that difference reads as a bug in
    // the counting.
    if (Report.ReturnsAbandoned != 0)
      m_result.Notes.push_back(
          formatv(
              "observation \"{0}\" is taken as the function returns, and "
              "{1} of its {2} calls left without returning -- an exception, "
              "a longjmp, or a thread that ended inside. Those calls "
              "produced no event; observe the function on entry to count "
              "all of them.",
              Site->Obs->Label, Report.ReturnsAbandoned, Site->Hits)
              .str());

    // The interleaving is not a fault, but every number here is per observation
    // rather than per thread, so a reader who assumes one thread will read a
    // change that is really two threads' values alternating as a change in one.
    if (Report.Threads > 1)
      m_result.Notes.push_back(
          formatv("observation \"{0}\" was hit on {1} threads. Its hit order, "
                  "its change detection and its aggregate cover the whole "
                  "observation rather than one thread, so the sequence is an "
                  "interleaving; the \"tid\" field on each event is what "
                  "separates it again.",
                  Site->Obs->Label, Report.Threads)
              .str());

    // Rare, and silent if it is not said: a frame whose return could not be
    // waited for produces no event, and neither does a gate that could not be
    // tracked, which leaves an empty observation looking like code that never
    // ran.
    if (const uint64_t Failures =
            Site->Returns.ArmFailures + Site->GateReturns.ArmFailures)
      m_result.Notes.push_back(
          formatv(
              "observation \"{0}\": {1} frames could not be watched for "
              "their return, because the address they return to or the "
              "frame itself could not be read. Those calls are missing from "
              "this observation rather than absent from the program.",
              Site->Obs->Label, Failures)
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
    Artifact.CarriesBacktrace = any_of(m_sites, [](const auto &Site) {
      return Site->Obs->Backtrace > 0;
    });
    m_result.Artifact = std::move(Artifact);
  }

  return std::move(m_result);
}
