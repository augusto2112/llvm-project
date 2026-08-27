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
#include "lldb/Breakpoint/BreakpointLocation.h"
#include "lldb/Breakpoint/StoppointCallbackContext.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/SourceManager.h"
#include "lldb/Host/Host.h"
#include "lldb/Host/PosixApi.h"
#include "lldb/Host/ProcessLaunchInfo.h"
#include "lldb/Symbol/CompilerType.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/LineEntry.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/ABI.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/FunctionPatch.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/StackID.h"
#include "lldb/Target/StopInfo.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/TargetList.h"
#include "lldb/Target/Thread.h"
#include "lldb/Target/ThreadList.h"
#include "lldb/Target/UnixSignals.h"
#include "lldb/Utility/Baton.h"
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
#include "llvm/Support/DJB.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
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

/// A value as the aggregate files it: the text it is counted under, and whether
/// that text is a serialized document rather than a bare scalar.
struct AggregatedValue {
  std::string Key;

  /// Set when \ref Key is a document. The aggregate is keyed by string -- that is
  /// what lets an emission mode and a comparison both decide "the same value" by
  /// comparing text -- so a composite value's document ends up as a map key, and
  /// the outer serialization then escapes every quote in it. `BB->getName()`, on
  /// any reading the most common thing an LLVM developer captures, came back as
  ///
  ///     "{\"Data\":{\"summary\":\"\\\"exit\\\"\"},\"Length\":{\"value\":\"4\"}} x3"
  ///
  /// which is a value the caller has to unescape by hand to read. Recording this
  /// is what lets the rendering put the document back where it belongs while the
  /// key stays a key. Nothing is stored twice: the key *is* the serialization, so
  /// the value is recovered by parsing it rather than by keeping a second copy per
  /// distinct value, of which a high-cardinality capture has one per hit.
  bool Document = false;
};

/// The text a value is aggregated under. A scalar serializes to {"value":"7"},
/// and using that document as the aggregate's key would spend three quarters of
/// the densest part of the response on repeated punctuation. Anything with
/// structure keeps it, since there the structure is the information.
///
/// Every capture goes through here, including `$return`: two spellings of the
/// same number are two values to an aggregate that compares them as strings, so
/// a run's return values would be summarised separately from the paths they came
/// from and neither would be comparable with the other.
AggregatedValue AggregateKey(const json::Value &V) {
  if (const json::Object *Obj = V.getAsObject()) {
    // A marker is not data. A value reduced for size carries its own value plus
    // an `_elided` saying so, and keying on the whole document repeated that
    // sentence in every histogram key, on both sides of every transition and in
    // every outlier -- which is what the reduction was meant to stop.
    //
    // `printed_as_value` puts the `printed` family in the same class. It says the
    // scalar under `value` is what the expression printed, so the raw per-stream
    // copy beside it is a second rendering of the same answer rather than another
    // field -- and keying on the pair would put the unbounded copy back into
    // every entry, which is the whole reason the text was promoted.
    if (std::optional<StringRef> Scalar = Obj->getString("value")) {
      const bool Promoted =
          Obj->getBoolean(PrintedAsValueField).value_or(false);
      const bool OnlyMarkers = all_of(*Obj, [Promoted](const auto &Entry) {
        const StringRef Key = Entry.first;
        return Key == "value" || Key.starts_with("_") ||
               (Promoted && Key.starts_with("printed"));
      });
      if (OnlyMarkers)
        return {Scalar->str(), /*Document=*/false};
    }
  }
  return {Compact(V), /*Document=*/true};
}

/// Whether a serialized value says the capture could not be read, rather than
/// saying what the program held there.
///
/// Such a value is an error about the capture and not a value the program took, so
/// it is kept out of the aggregate: as a histogram entry it competes with the real
/// values for a bounded list, as a transition it reports a change the program
/// never made, and as an outlier it is the rare value a caller is told to read
/// first. One broken capture polluted all three. It is reported once, at top
/// level, in `capture_failures`.
bool IsUnavailable(const json::Value &V) {
  const json::Object *Obj = V.getAsObject();
  return Obj && Obj->get("unavailable");
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

bool lldb_private::mcp::IsCeilingStop(Outcome O) {
  return O == Outcome::TimedOut || O == Outcome::NoProgress;
}

std::string lldb_private::mcp::DescribeWallClock(double RunningMs,
                                                double SetupMs) {
  return formatv(" ({0:F1}s running after {1:F1}s of setup, which the ceiling "
                 "does not count)",
                 RunningMs / 1000.0, SetupMs / 1000.0)
      .str();
}

unsigned lldb_private::mcp::TerminalLocalRank(StringRef Name, bool IsScalar,
                                              bool IsArgument) {
  // Tested before anything else, so that a generated temporary goes last whatever
  // its type is: a `__range1` is an aggregate whose expansion nobody asked for,
  // and a `__begin2` is an iterator nobody will name in a capture.
  if (Name.starts_with("__"))
    return 4;
  if (Name == "this")
    return 3;
  if (IsScalar)
    return 0;
  return IsArgument ? 2 : 1;
}

std::string lldb_private::mcp::ElidedLocals(ArrayRef<StringRef> Starved,
                                            size_t NotRead) {
  /// Starved locals named. A name is what a caller needs in order to capture the
  /// value on the next run; the ninth name is not a ninth thing to do.
  constexpr size_t MaxNamed = 8;

  std::string Out;
  if (!Starved.empty()) {
    // The count first, because it is what says whether the names are the whole
    // list, and then as many names as the bound allows. No advice after them: at
    // three locals a sentence telling the caller to capture one cost more than
    // the per-local markers this replaces, and `capture` is where a reader of the
    // documentation already knows to go.
    Out = formatv("no budget for {0} locals: ", Starved.size()).str();
    for (size_t I = 0, E = std::min(Starved.size(), MaxNamed); I < E; ++I) {
      if (I != 0)
        Out += ", ";
      Out += Starved[I];
    }
    if (Starved.size() > MaxNamed)
      Out += ", ...";
  }
  if (NotRead != 0) {
    if (!Out.empty())
      Out += ". ";
    // Not named, because these are past the bound on how many locals are read at
    // all: nothing here has read them, so there is no name to give.
    Out += formatv("{0} more locals not read", NotRead).str();
  }
  return Out;
}

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

StringRef lldb_private::mcp::ToString(CaptureFailure Kind) {
  switch (Kind) {
  case CaptureFailure::Situational:
    return "not_available_here";
  case CaptureFailure::UnknownName:
    return "no_such_name";
  case CaptureFailure::UnknownMember:
    return "no_such_member";
  case CaptureFailure::Malformed:
    return "malformed";
  }
  llvm_unreachable("unhandled CaptureFailure");
}

bool lldb_private::mcp::IsStableFailure(CaptureFailure Kind) {
  switch (Kind) {
  case CaptureFailure::Situational:
    return false;
  case CaptureFailure::UnknownName:
  case CaptureFailure::UnknownMember:
  case CaptureFailure::Malformed:
    return true;
  }
  llvm_unreachable("unhandled CaptureFailure");
}

CaptureFailure lldb_private::mcp::ClassifyCaptureFailure(StringRef Diagnostic) {
  // Matched on the compiler's own wording. Fragile against a diagnostic being
  // reworded, and the failure mode of that is a capture that keeps being
  // retried rather than one wrongly abandoned -- which is why the default is
  // Situational and why nothing here decides anything but how soon to stop.
  auto Says = [&](StringRef Text) { return Diagnostic.contains(Text); };

  // The base of the path resolved; the type it named has no such member. A
  // library loading later cannot add one.
  //
  // Tested before the shape cases because one expression draws both, and the
  // compiler emits them the other way round: a misspelled member reached with a
  // dot through a pointer is objected to first as a dot on a pointer and only
  // then as a member the type does not have. The member name is the fault --
  // applying the arrow reaches the same type and still finds no such member --
  // and it is also the reading with somewhere useful to look for candidates.
  if (Says("no member named") || Says("does not have a member named") ||
      Says("has no member named"))
    return CaptureFailure::UnknownMember;

  // A member read out of something with no members, or a subscript of
  // something that is not indexable: a fault in the shape of the expression
  // against the types at hand, not in any name in it.
  if (Says("is not a structure or union") ||
      Says("is not a class, struct, or union") ||
      Says("subscripted value is not an array") ||
      // A pointer/dot confusion carries a fixit, so reaching here means the
      // fixit did not repair it either.
      Says("did you mean to use '->'") || Says("did you mean to use '.'") ||
      Says("expected expression") || Says("expected ')'") ||
      Says("expected ';'") || Says("expected unqualified-id"))
    return CaptureFailure::Malformed;

  // Nothing of that name is in scope. Stable for as long as the module list is,
  // which is what the caller of this has to account for.
  if (Says("use of undeclared identifier") || Says("undeclared identifier") ||
      Says("cannot find") || Says("use of unresolved identifier"))
    return CaptureFailure::UnknownName;

  return CaptureFailure::Situational;
}

std::string lldb_private::mcp::DescribeCaptureFailure(StringRef Diagnostic,
                                                      CaptureFailure Kind,
                                                      unsigned MaxChars) {
  // The line that decided the class, where the message holds more than one
  // objection. Reporting the first one instead pairs a reason with a detail
  // about something else -- "no_such_member" beside a sentence about arrows --
  // which reads as the classification having gone wrong.
  SmallVector<StringRef, 8> Lines;
  Diagnostic.split(Lines, '\n');
  for (StringRef Line : Lines)
    if (Line.contains("error:") && ClassifyCaptureFailure(Line) == Kind)
      return CondenseDiagnostic(Line, MaxChars);

  // No line owns the class on its own -- a Situational failure has no marker to
  // match, and an execution error is one line that is itself the diagnostic.
  return CondenseDiagnostic(Diagnostic, MaxChars);
}

CaptureCandidates
lldb_private::mcp::RankCaptureCandidates(StringRef Wanted,
                                         std::vector<std::string> InScope) {
  CaptureCandidates Result;
  if (InScope.empty())
    return Result;

  // Sorted before ranking because the ranking is stable and leaves equally
  // close names in the order given, which is only a useful order if it is
  // alphabetical.
  llvm::sort(InScope);
  InScope.erase(std::unique(InScope.begin(), InScope.end()), InScope.end());
  Result.InScope = InScope.size();

  // Only the last component of the capture is compared. A caller who wrote
  // `Node.chidren` has the base right and the member wrong, and comparing the
  // whole expression against a bare field name would rank every field equally
  // far away.
  StringRef Base = Wanted;
  const size_t Split = Base.find_last_of(".>[");
  if (Split != StringRef::npos)
    Base = Base.drop_front(Split + 1);

  SmallSet<StringRef, 8> Taken;
  for (StringRef Near : NearestNames(Base, InScope, MaxCaptureCandidates)) {
    Result.Names.push_back(Near.str());
    Taken.insert(Near);
  }

  // Then whatever else was there, up to the bound. A caller who misremembered a
  // name rather than mistyping it gets no near match at all, and the list of
  // what the frame actually held is the answer for them.
  for (const std::string &Name : InScope) {
    if (Result.Names.size() >= MaxCaptureCandidates)
      break;
    if (!Taken.contains(StringRef(Name)))
      Result.Names.push_back(Name);
  }
  return Result;
}

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
  // early is worth more than the chance that the name would have resolved
  // later.
  //
  // One failure is enough when the failure is stable. The attempts exist to let
  // a value that is merely absent at this hit appear at the next one, and a
  // member a type does not have is not absent -- it is not a member. Waiting
  // three hits to say so costs two more compiles and delays the report of a
  // fault the caller can fix.
  //
  // Except for a name out of scope where the tracepoint has more than one
  // location: a name is looked up at a program counter, differently inlined
  // copies of one function have different variables in scope, and a capture
  // dropped at the first hit would be lost at every later location that did
  // hold it. Losing data is worse than two compiles, so that case keeps its
  // attempts.
  const bool SettledByOneFailure =
      IsStableFailure(In.Failure) &&
      (In.Failure != CaptureFailure::UnknownName || In.Locations <= 1);
  const uint64_t Attempts =
      SettledByOneFailure ? 1 : UnresolvableCaptureAttempts;
  if (In.Tier == ValueResolutionTier::Unresolved && In.Errors >= Attempts &&
      In.Errors == In.ObservedHits) {
    D.Disable = true;
    // A return observation has a known cause, and naming the scope instead would
    // send the caller hunting a declaration that is in the right place. Measured
    // on a live run: eight captures of a predicate's own locals, taken at its
    // return, were reported as too expensive with advice to spell them more
    // cheaply -- which a bare identifier cannot be. The remedy differs by what
    // the name is, and "on": "entry" is only right for a parameter: a local is
    // not assigned yet at entry, where it reads as uninitialised stack or as the
    // previous call's leftover value, which is plausible and wrong.
    // Phrased without naming the expression, so that captures stopped for the
    // same reason share one note and are listed against it. The reason here is a
    // fact about where the observation is taken rather than about any one of
    // them, and a run whose eight captures all failed at a return site spent
    // 2.6 kB saying so eight times, each copy differing only in the name that the
    // key it was filed under already gave.
    if (In.AtReturn)
      D.Note =
          formatv("stopped after {0} hits with no value: this observation is "
                  "taken as the function returns, where its frame has "
                  "already been popped, so the function's own parameters and "
                  "locals cannot be read. Capture \"{1}\" for what it "
                  "produced; read a parameter with \"on\": \"entry\", and a "
                  "local at a source location past its assignment, \"at\": "
                  "\"file.cpp:LINE\", since at entry it holds whatever was "
                  "on the stack.",
                  In.Errors, "$return")
              .str();
    else if (SettledByOneFailure)
      // Short, and it does not repeat the compiler. The reason, the candidate
      // names and the fixit are reported together in "capture_failures", where
      // they are real JSON rather than prose; saying it twice would put the
      // longer copy in the place a reader reaches second.
      D.Note = "stopped at the first failure: this cannot resolve at any hit, "
               "so retrying it would only cost compiles. See "
               "\"capture_failures\" for the compiler's reason and the names "
               "that were in scope.";
    else
      D.Note =
          formatv("stopped after {0} hits with no value, so the name does "
                  "not resolve where this observation is taken. Check that "
                  "it is in scope there -- a local declared further down the "
                  "function, or a member of another object, resolves nowhere "
                  "at this line -- rather than spelling it more cheaply.",
                  In.Errors)
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
  // "after N hits" rather than "after N of M hits". The M available here is the
  // hits so far, so the pair always read "after 66 of 66 hits" -- a fraction of
  // one, from a run that went on to have four thousand. The share is reported
  // instead by \ref CaptureCostDecision::Render, whose numbers are corrected once
  // the run's own total is known.
  OS << formatv("stopped evaluating \"{0}\" after {1} hits: {2:F2} ms "
                "per hit projects {3:F2} ms over the {4:F2} ms left, which is "
                "more than the 1/{5} of it one capture may take.",
                In.Expr, In.ObservedHits, D.PerHitMs, D.ProjectedMs,
                D.RemainingMs, CaptureCostBudgetDivisor);
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
  // Without the note, which is rendered once per observation rather than once
  // per capture. Measured on an observation whose eight captures were all
  // stopped for the same reason: 2.6 kB of one sentence, said eight times over,
  // differing only in the expression each quoted -- which is the key it was
  // filed under.
  return json::Object{
      {"observed_hits", static_cast<int64_t>(ObservedHits)},
      {"total_hits", static_cast<int64_t>(TotalHits)},
      {"per_hit_ms", Round2(PerHitMs)},
      {"projected_ms", Round2(ProjectedMs)},
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
lldb_private::mcp::RankFrames(ArrayRef<RawFrame> Frames, uint32_t *RawDropped) {
  if (RawDropped)
    *RawDropped = 0;

  // Folding comes first and runs over unwinder order, because adjacency is what
  // makes a repeated name recursion rather than a coincidence.
  std::vector<RankedFrame> Folded;
  for (const RawFrame &Frame : Frames) {
    if (!Folded.empty() && Folded.back().Function == Frame.Function) {
      RankedFrame &Run = Folded.back();
      ++Run.Repeats;
      // A run entered through more than one call site carries the location
      // nearest the failure, which is the innermost one to resolve a file. The
      // index follows the location: it exists so that what is read out of the
      // frame agrees with what the entry says about it.
      if (Run.File.empty() && !Frame.File.empty()) {
        Run.File = Frame.File;
        Run.Line = Frame.Line;
        Run.Index = Frame.Index;
        Run.IsSystem = IsSystemSourcePath(Run.File);
      }
      continue;
    }

    RankedFrame Ranked;
    Ranked.Function = Frame.Function;
    Ranked.File = Frame.File;
    Ranked.Line = Frame.Line;
    Ranked.Index = Frame.Index;
    Ranked.IsSystem = IsSystemSourcePath(Frame.File);
    Folded.push_back(std::move(Ranked));
  }

  std::vector<RankedFrame> WithSource;
  uint32_t Dropped = 0;
  for (const RankedFrame &Frame : Folded)
    if (Frame.File.empty())
      Dropped += Frame.Repeats;
    else
      WithSource.push_back(Frame);

  // Keeping the sourceless frames when none has source is what stops a stripped
  // binary from reporting no location at all. Where the program is remains the
  // answer even where the source does not exist.
  std::vector<RankedFrame> Kept;
  if (WithSource.empty()) {
    Kept = std::move(Folded);
  } else {
    Kept = std::move(WithSource);
    if (RawDropped)
      *RawDropped = Dropped;
  }

  // Stable, so that within each group the innermost frame stays innermost:
  // among the program's own frames, order is the answer.
  llvm::stable_sort(Kept, [](const RankedFrame &A, const RankedFrame &B) {
    return static_cast<int>(A.IsSystem) < static_cast<int>(B.IsSystem);
  });
  return Kept;
}

std::string lldb_private::mcp::CollapseTemplateArguments(StringRef Function) {
  std::string Out;
  Out.reserve(Function.size());
  unsigned Depth = 0;
  for (size_t I = 0, E = Function.size(); I != E; ++I) {
    const char C = Function[I];

    // `operator<`, `operator<=>` and `operator<<` are names, not argument lists,
    // and a `<` that opens a list is always preceded by an identifier character.
    if (C == '<' && Depth == 0) {
      StringRef Before = Function.take_front(I);
      if (Before.ends_with("operator") || Before.ends_with("operator<"))
        Out += C;
      else {
        Out += "<...>";
        ++Depth;
      }
      continue;
    }

    if (Depth != 0) {
      // Counted rather than matched, so that a nested list is elided along with
      // the one that encloses it instead of ending it early.
      if (C == '<')
        ++Depth;
      else if (C == '>')
        --Depth;
      continue;
    }
    Out += C;
  }
  return Out;
}

namespace {

/// Characters a shared directory prefix has to reach before naming it once pays
/// for the field that names it.
///
/// Measured on a compiler backtrace of 14 frames over 7 distinct files: the
/// `file` keys and values were 1,478 of the 3,207 characters the frame list cost,
/// and a 65-character build-directory prefix appeared in every one of them. It is
/// the same argument template arguments are collapsed by -- what the repetition
/// distinguishes between the entries is nothing, because it is identical in all
/// of them.
constexpr size_t MinSharedFileRoot = 24;

/// The directory every path in \p Files lies under, or empty when they share too
/// little of one for naming it to pay.
///
/// Compared component by component rather than character by character, so the
/// answer is always a directory and never a prefix stopping halfway through the
/// name of one: sibling directories `slot-3` and `slot-30` share six characters
/// and no directory at all, and a reader joining that prefix back onto a relative
/// path would name a file that does not exist.
std::string CommonSourceDirectory(ArrayRef<StringRef> Files) {
  if (Files.size() < 2)
    return std::string();

  const StringRef First = sys::path::parent_path(Files.front());
  SmallVector<StringRef, 16> Shared;
  for (auto It = sys::path::begin(First), E = sys::path::end(First); It != E;
       ++It)
    Shared.push_back(*It);

  for (const StringRef File : Files.drop_front()) {
    const StringRef Dir = sys::path::parent_path(File);
    size_t Common = 0;
    auto It = sys::path::begin(Dir), E = sys::path::end(Dir);
    for (; Common < Shared.size() && It != E && *It == Shared[Common];
         ++Common, ++It) {
    }
    Shared.truncate(Common);
    if (Shared.empty())
      return std::string();
  }

  // Cut out of the path the components were walked from rather than rebuilt by
  // joining them, so the root is spelled with the separators the program's own
  // paths use.
  const StringRef Last = Shared.back();
  const StringRef Root =
      First.take_front(Last.data() - First.data() + Last.size());
  if (Root.size() < MinSharedFileRoot)
    return std::string();
  return Root.str();
}

/// \p File with \p Root and the separator after it removed, or \p File whole
/// when it does not lie under \p Root.
std::string RelativeToRoot(StringRef File, StringRef Root) {
  if (Root.empty() || !File.starts_with(Root))
    return File.str();
  StringRef Rest = File.drop_front(Root.size());
  while (!Rest.empty() && sys::path::is_separator(Rest.front()))
    Rest = Rest.drop_front();
  // A path that is the root itself keeps its own spelling, because a relative
  // path of nothing names no file.
  if (Rest.empty())
    return File.str();
  return Rest.str();
}

} // namespace

json::Value RankedFrame::Render(StringRef FileRoot) const {
  json::Object O{{"function", Function}};
  if (!File.empty()) {
    O["file"] = RelativeToRoot(File, FileRoot);
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
  if (S.Samples == 0) {
    S.Function = Innermost.Function;
    S.File = Innermost.File;
    S.Line = Innermost.Line;
    S.Tid = Tid;
    S.FirstSample = m_samples;
  }
  ++S.Samples;

  // Counted once per sample however many frames of it a recursive function holds,
  // so that "in nine samples out of ten" means that rather than "on the stack
  // ninety times".
  SmallSet<StringRef, 32> Counted;
  for (const auto &[Depth, Frame] : llvm::enumerate(InnermostFirst)) {
    if (!Counted.insert(Frame.Function).second)
      continue;
    Coverage &C = m_coverage[{Tid, Frame.Function}];
    ++C.Samples;
    C.DepthSum += Depth;
  }
}

lldb::tid_t StackProfile::BusiestThread() const {
  /// What one thread's samples hold, which is what separates a thread doing work
  /// from a thread that merely existed for the same length of time.
  struct Work {
    /// Samples whose innermost frame resolved to source.
    uint64_t Resolved = 0;

    /// Distinct innermost frames, which is one for a thread parked in a wait
    /// however long the run lasted.
    uint64_t Sites = 0;

    /// Samples the thread was alive across, which is all this used to rank on.
    uint64_t Alive = 0;
  };

  std::map<lldb::tid_t, Work> Threads;
  for (const auto &[Key, S] : m_sites) {
    Work &W = Threads[Key.first];
    if (!S.File.empty())
      W.Resolved += S.Samples;
    ++W.Sites;
  }
  for (auto &[Tid, W] : Threads)
    if (auto It = m_per_thread.find(Tid); It != m_per_thread.end())
      W.Alive = It->second;

  lldb::tid_t Busiest = 0;
  Work Best;
  // Ascending thread id, with a strict comparison, so that a genuine tie is
  // resolved the same way every run rather than by whatever order the map was
  // built in.
  for (const auto &[Tid, W] : Threads)
    if (Busiest == 0 || std::tie(W.Resolved, W.Sites, W.Alive) >
                            std::tie(Best.Resolved, Best.Sites, Best.Alive)) {
      Busiest = Tid;
      Best = W;
    }
  return Busiest;
}

json::Value StackProfile::Render() const {
  // Too few samples to be a profile rather than a coincidence.
  if (m_samples < MinProfileSamples)
    return nullptr;

  // A profile of a program that was never sampled inside its own code says
  // nothing about that program. Measured on a run that crashed a fifth of a
  // second after launch: the only samples due were taken while the dynamic loader
  // was still mapping images, and a section reporting that as where the program
  // spent its time is worse than no section.
  if (llvm::all_of(m_sites,
                   [](const auto &Entry) { return Entry.second.File.empty(); }))
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
  size_t Kept = std::min<size_t>(Ranked.size(), MaxHot);
  // Where no place holds a share of the run, which place was innermost is a fact
  // about the last instruction of an accessor rather than about the program. One
  // entry keeps a file and a line to look at; `under` carries the answer.
  //
  // Two tests, because a share alone has no floor: two samples of sixteen is an
  // eighth exactly and passes, and the run it came from had nothing to rank.
  if (Ranked.front()->Samples * MinHotShareDivisor < m_samples ||
      Ranked.front()->Samples < MinHotSamples)
    Kept = 1;
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

  // Where the run was, which is a different question from which function the
  // innermost frame was in. A function on the stack for half the samples is where
  // the program is spending itself whether or not it is ever the leaf, and in an
  // unoptimized build it never is: measured on a compiler looping inside one
  // analysis, the eight hottest sites were `isPresent`, `capacity`, `getValueID`
  // and other one-line accessors with one or two samples each, while the function
  // actually spinning appeared in every sample and in none of them as the leaf.
  //
  // Of the busiest thread's samples, because two threads share nothing but the
  // bottom of the stack and a path common to both would describe neither. Which
  // thread that is is a question about what the samples hold, not about how many
  // there were: see \ref BusiestThread.
  const lldb::tid_t Busiest = BusiestThread();
  uint64_t Best = 0;
  if (auto It = m_per_thread.find(Busiest); It != m_per_thread.end())
    Best = It->second;

  struct Covering {
    StringRef Function;
    double MeanDepth;
  };
  std::vector<Covering> Path;
  for (const auto &[Key, C] : m_coverage) {
    if (Key.first != Busiest || C.Samples * UnderShareDivisor < Best)
      continue;
    Path.push_back({Key.second, static_cast<double>(C.DepthSum) /
                                    static_cast<double>(C.Samples)});
  }
  // Innermost first, matching the order a backtrace is reported in. A callee is
  // deeper than its caller in every sample holding both, so the means order the
  // chain even where the counts tie.
  llvm::stable_sort(Path, [](const Covering &LHS, const Covering &RHS) {
    return LHS.MeanDepth < RHS.MeanDepth;
  });
  if (Path.size() > MaxUnder)
    Path.resize(MaxUnder);
  json::Array Under;
  for (const Covering &Frame : Path)
    Under.push_back(Frame.Function);
  if (!Under.empty()) {
    O["under"] = std::move(Under);
    // Which thread the path belongs to. `hot` carries a `tid` per entry and this
    // carried none, so nothing in the response said that the two describe
    // different threads -- and on a program with one thread spinning and several
    // parked, that is the whole of what a reader is trying to establish.
    if (Threaded)
      O["under_tid"] = static_cast<int64_t>(Busiest);
  }
  return O;
}

//===----------------------------------------------------------------------===//
// Result rendering
//===----------------------------------------------------------------------===//

json::Value CaptureReport::Render() const {
  // How many hits this capture came back with a value at, said on the row rather
  // than left to be worked out.
  //
  // Every short form here is a bare word naming how the capture resolved, and a
  // word says nothing about whether it then read. Absent the count the reader has
  // to infer that from an omission -- no `errors` field, so no failures, so as
  // many reads as the enclosing observation had hits -- which is what a silently
  // failing capture also looks like. A hand-written path into another project's
  // internals is the case where that matters: it is the kind of capture most
  // likely to resolve and read nothing, and measured on a run of them the
  // response was read as "the field was uninteresting" and answered by capturing
  // four fields redundantly so they could corroborate each other, at four times
  // the cost of the answer.
  //
  // Spelled as the aggregate spells a value that never varied, `"false x4012"`:
  // this response already has an idiom for a thing and how many times, and six
  // characters is what the whole gap costs to close.
  const uint64_t Read = Evaluations >= Errors ? Evaluations - Errors : 0;
  const std::string Times = formatv(" x{0}", Read).str();

  // `$return` is read out of the ABI's result location rather than resolved
  // from a name, so it has no tier. It is also left out of `capture_failures`,
  // there being no name to correct, so the count is the only thing that
  // distinguishes a return value read at every hit from one never readable at
  // all -- previously both were the word "abi".
  if (FromABI) {
    if (Evaluations == 0)
      return "not_evaluated";
    if (Errors == 0)
      return "abi" + Times;
    return json::Object{{"tier", "abi"},
                        {"evaluations", static_cast<int64_t>(Evaluations)},
                        {"errors", static_cast<int64_t>(Errors)}};
  }

  // A capture the program recorded for itself has no tier and no cost for the
  // same reason: no name was resolved and no expression was run. It is also the
  // one word here that says the hit it came from cost nothing at all -- the
  // observation's own `eval` says the tracepoint's work is in the program, and
  // this says the values are too, so nothing stopped.
  if (InProcess) {
    if (Evaluations == 0)
      return "not_evaluated";
    if (Errors == 0)
      return "in_process" + Times;
    return json::Object{{"tier", "in_process"},
                        {"evaluations", static_cast<int64_t>(Evaluations)},
                        {"errors", static_cast<int64_t>(Errors)}};
  }

  // A capture that resolved as a path and never failed has nothing to say
  // beyond how it resolved and how often, and most captures are that.
  if (!Disabled && Errors == 0 && FixedExpr.empty() &&
      Tier == ValueResolutionTier::VariablePath)
    return ToString(Tier).str() + Times;

  // Never evaluated is not the same as could not be read: the observation may
  // never have been hit, or its condition never held. The default tier renders
  // "unavailable", which is the word an expression that genuinely failed gets,
  // so reporting it here would collapse the distinction the rest of plan_report
  // is built to keep.
  //
  // Said as a word rather than as an object, because the numbers beside it are all
  // zero and a capture on an observation that never fired has nothing else to
  // report: `{"evaluations":0,"tier":"not_evaluated","total_ms":0}` is three fields
  // saying what one says.
  if (Evaluations == 0 && !Disabled && Errors == 0 && FixedExpr.empty())
    return "not_evaluated";

  const std::string TierName = ToString(Tier).str();

  json::Object O{{"tier", TierName},
                 {"evaluations", static_cast<int64_t>(Evaluations)},
                 {"total_ms", Round2(TotalMs)}};

  // The run is not evaluating what the caller wrote, and nothing else in the
  // response says so. Beside the key, which is the caller's own spelling, this
  // is what connects the two.
  if (!FixedExpr.empty())
    O["fixed_as"] = FixedExpr;
  if (Errors != 0)
    O["errors"] = static_cast<int64_t>(Errors);
  if (Disabled)
    O["disabled"] = Disabled->Render();
  return O;
}

json::Value CaptureFailureReport::Render() const {
  json::Object O{
      {"observation", Label}, {"capture", Expr}, {"reason", ToString(Kind)}};

  // A condition is not a capture: it decides whether a hit is recorded at all,
  // so one that cannot be evaluated leaves an observation looking like code
  // that never ran. Named rather than implied.
  if (IsCondition)
    O["field"] = "when";

  if (!Reason.empty())
    O["detail"] = Reason;
  if (!FixedExpr.empty()) {
    O["fixed_as"] = FixedExpr;
    // A fixit that was suggested and failed as well is not a fix, and a caller
    // who read it as one would adopt a spelling that never resolved.
    O["fix_applied"] = FixApplied;
  }
  if (!Candidates.Names.empty()) {
    json::Array Names;
    for (const std::string &Name : Candidates.Names)
      Names.push_back(Name);
    O["candidates"] = std::move(Names);
    // What the list was drawn from, so that six names read as a selection from
    // forty rather than as everything there was.
    if (Candidates.InScope > Candidates.Names.size())
      O["candidates_of"] = static_cast<int64_t>(Candidates.InScope);
  }
  O["disabled"] = Disabled;
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
  if (SourceNewerThanBinary)
    O["source_newer_than_binary"] = *SourceNewerThanBinary;

  // Reported only for an observation that carried a condition, so that their
  // absence says "no condition" rather than "a condition that never held".
  if (HasCondition) {
    O["condition_true"] = static_cast<int64_t>(ConditionTrue);
    O["condition_errors"] = static_cast<int64_t>(ConditionErrors);
    O["condition_ms"] = Round2(ConditionMs);
  }

  // Said per observation because the answer differs per observation: one
  // tracepoint's condition may be compiled in while another's function had no
  // source to recompile. A caller comparing hit counts between runs needs to
  // know which of them paid for a stop per hit.
  //
  // Under the test that says there was something to compile at all. A bare hit
  // counter has neither a condition to test nor a value to read, so there is
  // nothing for the program to do for itself and nothing to report about it --
  // whereas for an observation that reads something, its absence would say the
  // question was never asked.
  if (HasCondition || !Captures.empty()) {
    if (InProcess)
      O["eval"] = "in-process";
    else if (FallbackReason.empty())
      O["eval"] = "stopped";
    else
      O["eval"] = "stopped: " + FallbackReason;
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

    // Grouped by reason, in the order the captures were given, so that a reason
    // holding for several of them is stated once and names them. The reason a
    // capture was stopped is usually a fact about where the observation is taken
    // rather than about the expression -- at a return site none of the
    // function's own names can be read -- and a per-capture rendering repeats it
    // once per name.
    std::vector<std::pair<std::string, json::Array>> ByReason;
    for (const CaptureReport &Capture : Captures) {
      if (!Capture.Disabled || Capture.Disabled->Note.empty())
        continue;
      std::string Note = Capture.Disabled->Note;

      // What the aggregate holds for a capture that was stopped partway, said
      // where the reader is told it was stopped. Without this the histogram is a
      // sample presented as a summary: measured on a printer disabled after 66
      // of 4000 hits, `values` came back as four buckets totalling 66 with
      // nothing anywhere saying the other 3934 hits were not in it, and the
      // counts read as the run's distribution. `hits` and `evaluations` sit in
      // different objects, so seeing it meant noticing that two numbers three
      // fields apart disagreed.
      //
      // Only for a capture that read something. One stopped because it never
      // resolved contributed nothing to the aggregate, so there is no partial
      // summary to warn about, and its own reason already says why.
      if (Capture.Errors != Capture.Evaluations &&
          Capture.Disabled->ObservedHits < Hits)
        Note += formatv(" What \"aggregate\" holds for it covers those {0} hits "
                        "and not the {1} this observation went on to have.",
                        Capture.Disabled->ObservedHits, Hits)
                    .str();

      auto It = llvm::find_if(
          ByReason, [&](const auto &Entry) { return Entry.first == Note; });
      if (It == ByReason.end())
        ByReason.push_back({std::move(Note), json::Array{Capture.Expr}});
      else
        It->second.push_back(Capture.Expr);
    }
    if (!ByReason.empty()) {
      json::Array Stopped;
      for (auto &[Note, Exprs] : ByReason)
        Stopped.push_back(
            json::Object{{"captures", std::move(Exprs)}, {"note", Note}});
      O["stopped"] = std::move(Stopped);
    }
  }
  return O;
}

json::Value TerminalEvent::Render() const {
  json::Object O{{"description", Description}};
  if (ExitStatus)
    O["exit_status"] = static_cast<int64_t>(*ExitStatus);
  if (!Function.empty())
    O["function"] = Function;

  // The directory the frames share, said once. A backtrace of a build tree is
  // the same absolute prefix once per frame, which distinguishes nothing between
  // them; what a reader needs an absolute path for is opening the file, and the
  // root plus a relative path is that path.
  //
  // Only from two frames up, where there is something to share, and only past a
  // length where the field that names it costs less than the repetition it
  // removes.
  std::string FileRoot;
  if (Frames.size() >= 2) {
    SmallVector<StringRef, 24> Paths;
    for (const RankedFrame &Frame : Frames)
      if (!Frame.File.empty())
        Paths.push_back(Frame.File);
    if (!File.empty())
      Paths.push_back(File);
    FileRoot = CommonSourceDirectory(Paths);
  }
  if (!FileRoot.empty())
    O["file_root"] = FileRoot;

  if (!File.empty()) {
    O["file"] = RelativeToRoot(File, FileRoot);
    if (Line != 0)
      O["line"] = static_cast<int64_t>(Line);
  }

  // Absent means zero, which is the case a reader assumes and the one a stack
  // with source at its innermost frame produces. Present, it is what a follow-up
  // run has to select before any of this frame's names resolve.
  if (FrameIndex != 0)
    O["frame"] = static_cast<int64_t>(FrameIndex);

  // Which thread this is, once there is more than one for it to be. Everything
  // under this key describes one thread, and with several running that is a
  // choice the report made rather than a fact about the program.
  if (ThreadCount > 1 && Tid != LLDB_INVALID_THREAD_ID)
    O["tid"] = static_cast<int64_t>(Tid);

  if (!Frames.empty()) {
    json::Array Rendered;
    for (const RankedFrame &Frame : Frames)
      Rendered.push_back(Frame.Render(FileRoot));
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

void InferiorOutput::Append(bool Stderr, StringRef Text, size_t Max) {
  std::string &Stream = Stderr ? Err : Out;
  Stream.append(Text.data(), Text.size());
  if (Stream.size() <= Max)
    return;
  // The tail is the part that says how far the program got, so the front is what
  // goes. Bounded per stream rather than over the pair, so that a program
  // chattering on stdout cannot push a diagnostic off stderr -- the two are read
  // for different reasons and one filling up says nothing about the other.
  Stream.erase(0, Stream.size() - Max);
  Truncated = true;
}

json::Value InferiorOutput::Render() const {
  if (Empty())
    return nullptr;
  json::Object O;
  if (!Out.empty())
    O["stdout"] = Out;
  if (!Err.empty())
    O["stderr"] = Err;
  return O;
}

PrintedValue lldb_private::mcp::PrintedAsValue(const InferiorOutput &Printed,
                                               size_t MaxChars) {
  PrintedValue Result;

  // One stream, or none. Two streams that both produced text have no order
  // between them -- one file and one pty, with nothing marking which write came
  // first -- so there is no single text for the value to be, and inventing an
  // order would report a sequence the program did not have. Such a hit keeps the
  // void marker and its `printed`, which is what it had before.
  if (!Printed.Out.empty() && !Printed.Err.empty())
    return Result;
  StringRef Raw =
      Printed.Out.empty() ? StringRef(Printed.Err) : StringRef(Printed.Out);

  std::string Folded;
  Folded.reserve(Raw.size());
  for (size_t I = 0, E = Raw.size(); I != E; ++I) {
    if (Raw[I] == '\r' && I + 1 != E && Raw[I + 1] == '\n')
      continue;
    Folded += Raw[I];
  }

  StringRef Text = StringRef(Folded).trim();

  // Whitespace and nothing else is not a value. It is also what a drain that
  // caught only the newline the program had left buffered looks like, and filing
  // that as a bucket of its own would put a value in the histogram that no
  // expression produced.
  if (Text.empty())
    return Result;

  if (Text.size() <= MaxChars) {
    Result.Text = Text.str();
  } else {
    // The hash covers the whole text, not the part that was dropped, so that the
    // key is a function of everything printed. Without it two dumps agreeing for
    // 300 characters and to the byte in length -- the same node with one operand
    // changed, which is exactly what a caller is looking for -- would be counted
    // as one value, and a histogram that merges two answers is worse than one
    // that splits one.
    Result.Text = Text.take_front(MaxChars).str();
    Result.Text += formatv("... (+{0} more chars, whole text hashes {1:x-8})",
                           Text.size() - MaxChars, djbHash(Text))
                       .str();
    Result.Shortened = true;
  }

  // A dump can carry anything the program had in a string, and a key that is not
  // valid UTF-8 is not serializable.
  if (!json::isUTF8(Result.Text))
    Result.Text = json::fixUTF8(Result.Text);
  return Result;
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

  if (const json::Object *Agg = Aggregate.getAsObject(); Agg && !Agg->empty()) {
    O["aggregate"] = Aggregate;

    // Beside the aggregate rather than inside it, and only where it qualifies
    // what is there: a caller testing one key learns whether every field under
    // it describes the program or a prefix of it. Absent means the program
    // reached its own end, which is the case every field is read as by default.
    if (AggregateCoversPrefix)
      O["aggregate_covers"] = "partial";
  }

  // Above the aggregate in the reading order that matters: a capture that could
  // not be read is a fault in the request, and the aggregate is where a caller
  // goes for an answer rather than for a correction. Reached the other way it
  // is an escaped JSON document nested under a label, which is where reading
  // stops.
  if (!CaptureFailures.empty()) {
    json::Array Rendered;
    for (const CaptureFailureReport &Failure : CaptureFailures)
      Rendered.push_back(Failure.Render());
    O["capture_failures"] = std::move(Rendered);
  }

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
  if (json::Value Written = Output.Render(); Written != nullptr)
    O["inferior_output"] = std::move(Written);
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

    /// The spelling actually evaluated, once a fixit has repaired the one given
    /// and the repair has resolved. Set at most once per run: the rewrite is a
    /// property of the expression and the types around it, and re-deciding it
    /// per hit would let the effective spelling drift.
    ///
    /// The aggregate and the hit tuples stay keyed on \ref Expr. A key is the
    /// caller's handle on what they asked for, it is fixed before the run
    /// starts, and keying on the fixed spelling would both introduce a key
    /// nobody wrote and split one histogram in two at whichever hit the fixit
    /// was found.
    std::string FixedExpr;

    /// A fixit that was offered and did not resolve either. Kept apart from
    /// \ref FixedExpr because it must not be adopted -- it is a suggestion the
    /// compiler could not make work -- and reported, because it is still the
    /// closest thing to a correction anyone has.
    std::string SuggestedFix;

    ValueResolutionTier Tier = ValueResolutionTier::Unresolved;
    uint64_t Evaluations = 0;
    uint64_t Errors = 0;
    Micros Spent{0};
    std::optional<CaptureCostDecision> Disabled;

    /// How the most recent failure was classified, and the evaluator's own
    /// account of it.
    CaptureFailure Failure = CaptureFailure::Situational;
    std::string FailureReason;

    /// Names that were in scope, gathered once at the first stable failure. Not
    /// per hit: the gathering walks debug info, and a capture failing the same
    /// way at every hit would pay for the same answer every time.
    CaptureCandidates Candidates;
    bool Diagnosed = false;

    /// Whether this capture has been seen to print, learned at the first hit it
    /// printed at.
    ///
    /// What it buys is a flush *before* the next evaluation as well as after it.
    /// One flush is unavoidable -- a buffered stream holds what an expression
    /// wrote until something empties it -- but a flush only after the expression
    /// also empties whatever the program itself had buffered and not yet written,
    /// and that text then arrives inside this capture's window and is credited to
    /// it. Flushing first is what separates the two, and it is paid only for a
    /// capture that has already been shown to print, since for every other one it
    /// would be an expression evaluation per hit that changes no answer.
    ///
    /// So the first printing hit of a capture is the imprecise one. That is the
    /// stated caveat rather than a thing to fix: knowing whether an expression
    /// prints before running it would mean reading the program's code.
    bool Prints = false;

    /// Modules loaded when the capture was turned off.
    ///
    /// A name that resolves nowhere now resolves once the library defining it
    /// is loaded, and that is the one thing that can make a stable failure stop
    /// being one. So the answer is allowed to change exactly when the thing
    /// that could change it does: a capture turned off is tried again after a
    /// module arrives, and reported as resolved if it then resolves. That is
    /// the rule an unresolved tracepoint location already follows by re-reading
    /// its location count after the run rather than before it.
    size_t DisabledAtModules = 0;

    /// Whether the program recorded this capture for itself. No name was
    /// resolved and no expression was run, so it has no tier and no cost, and
    /// the hit it came from cost no stop.
    bool InProcess = false;
  };
  std::vector<CaptureState> Captures;

  /// The `when` condition's adopted fixit, and how it last failed. A condition
  /// decides whether a hit is recorded at all, so one that cannot be evaluated
  /// leaves the observation reporting hits and no values -- which reads as code
  /// that ran and held nothing rather than as a condition that never worked.
  std::string WhenFixedExpr;
  std::string WhenSuggestedFix;
  CaptureFailure WhenFailure = CaptureFailure::Situational;
  std::string WhenFailureReason;
  CaptureCandidates WhenCandidates;
  bool WhenDiagnosed = false;

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

  /// Whether the sites this one enables have been opened, which happens at the
  /// first hit this observation records. Kept as a state rather than tested
  /// against a hit number, because the hits are counted in two places once the
  /// condition is compiled in.
  bool Opened = false;

  /// The compiled-in site this observation's condition was installed as, unset
  /// while the condition is being evaluated at a stop instead.
  std::optional<uint32_t> CompiledIn;

  /// Whether that site records this observation's captures and so never stops.
  /// The values arrive at a drain instead, and everything a hit does with them
  /// happens there.
  bool RecordsWithoutStopping = false;

  /// The function the copy was compiled from, which is the only thing a hit read
  /// out of a record knows about where it happened: no frame was ever current
  /// for it, because nothing stopped.
  std::string CompiledInFunction;

  /// One hit of this site whose captures have not all arrived.
  ///
  /// A thread held still partway through a hit has written some of that hit's
  /// values and not the rest, so a hit can straddle two reads. Held until it is
  /// complete rather than reported short, since the rest usually arrives at the
  /// next read -- and a tuple missing a value is a different hit as far as
  /// change detection and a comparison are concerned.
  struct PartialHit {
    /// As the site's own counter numbered it. Records carry it because two
    /// threads inside one site interleave theirs.
    uint64_t Hit = 0;

    /// By capture position, with the ones not yet recorded left null.
    std::vector<lldb::ValueObjectSP> Values;

    uint32_t Filled = 0;
  };

  /// In arrival order, so that hits reach the report in the order the program
  /// wrote them.
  std::vector<PartialHit> Partial;

  /// This observation's counts as of the moment its condition was compiled in.
  /// The injected code's own counters start from zero there, so the two halves
  /// have to be added rather than one of them read.
  uint64_t HitsBeforeCompile = 0;
  uint64_t ConditionTrueBeforeCompile = 0;

  /// What the injected code has counted, read back while the program is still
  /// there to read it from.
  uint64_t CompiledInHits = 0;
  uint64_t CompiledInConditionTrue = 0;

  /// Times the location was reached, wherever the count was kept. A hit whose
  /// condition was false never stops once the condition is compiled in, so the
  /// program's own counter is the only place it was ever recorded.
  uint64_t TotalHits() const {
    return CompiledIn ? HitsBeforeCompile + CompiledInHits : Hits;
  }

  uint64_t TotalConditionTrue() const {
    return CompiledIn ? ConditionTrueBeforeCompile + CompiledInConditionTrue
                      : ConditionTrue;
  }

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
///
/// Measured on a compiler stopped at an assertion: of 20 frames, the 7 innermost
/// were the fault and the rest were the pass manager reaching them through
/// alternating adaptor layers, which fold into nothing because no two are the same
/// function. Two agents asked for the top four. `frames_total` and
/// `frames_omitted` say what a bound left out, so a shorter list is not a quieter
/// one.
constexpr size_t MaxTerminalFrames = 16;

/// Per-hit capture tuples kept for a comparison. A tuple is the run's own record
/// of what a hit saw, and four thousand of them is enough to find where two runs
/// diverge in every case observed; past that the count of what was not kept is
/// what stops "identical" from overclaiming.
constexpr size_t MaxComparedHits = 4096;

/// Hits whose recorded values are still incomplete that one observation will hold
/// while the program runs.
///
/// A hit waits only for the values of it that a thread had not finished writing
/// when the program was held still, so under ordinary conditions there is at most
/// one of these per thread. The bound exists for the case where the rest of a
/// hit's values were overwritten before they could be read and are never coming:
/// such a hit is filed as it stands rather than held, so that the queue cannot
/// grow by one for every value the ring lost.
constexpr size_t MaxPartialHits = 256;

/// Locals the terminal event reports.
constexpr size_t MaxTerminalLocals = 32;

/// Characters one capture's value may render as. A capture is read at every hit
/// and its rendering appears three times in a summary -- as a histogram key, on
/// each side of a transition, and in the artifact -- so a value that is large
/// once is large many times over.
constexpr unsigned MaxCaptureChars = 300;

/// Characters of the evaluator's account of a failure that are reported.
///
/// A clang parse failure carries the evaluator describing itself, the source
/// line, caret art and one diagnostic per objection, of which the `error:` line
/// is the answer. \ref CondenseDiagnostic reduces it to that line; this bounds
/// what is left, because one objection can still name a fully-qualified
/// template type twice. Said once per failing capture rather than once per hit,
/// so the bound is about a reader's attention rather than about the response
/// size.
constexpr unsigned MaxFailureReasonChars = 400;

/// Value-tree nodes the terminal event's locals may spend between them. Sized so
/// that a frame of scalars and small structs comes back whole, while one local
/// that reaches an arbitrarily large object cannot crowd out the rest.
constexpr unsigned MaxTerminalLocalNodes = 96;

/// Nodes any one of those locals may take out of that budget.
///
/// The shared budget bounds the response; this bounds one local's share of it,
/// which is a different job. Without the second, the first local to reach a large
/// object spends the response on itself and every local after it is an elision
/// marker: measured on a compiler stopped inside one of its passes, `this` took 73
/// of the 96 nodes and left 25 locals with none. Twelve is a small struct or a
/// pointer and its members whole, and the loop-carried scalars a hang is explained
/// by cost one apiece.
constexpr unsigned MaxNodesPerTerminalLocal = 12;

/// Characters one of those locals may render as before it collapses to its own
/// value and a note of what was left out.
///
/// Larger than a capture's 300 because a terminal event happens once per run,
/// where a capture's rendering is repeated into a histogram and into each side of
/// every transition it appears in. The terminal was the only caller leaving this
/// unbounded, which is how one local came back as 4,440 characters of pass and
/// target state.
constexpr unsigned MaxTerminalLocalChars = 600;

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

/// How long a drain waits for what an expression printed to reach the debugger.
///
/// The program writes down a pipe that LLDB reads on a thread of its own, so a
/// write that has completed inside the process is not readable here at the
/// instant it returns. Without a wait the usual result is nothing at all:
/// measured on a capture calling a printer three times, the drain immediately
/// after each call read zero bytes every time, and the text turned up in the
/// run's own output at the end.
///
/// Bounded, and spent only where the capture is one that prints -- see the call
/// site -- because a tracepoint hit thousands of times would otherwise pay it at
/// every hit for output that was never coming.
constexpr Micros AttributedOutputWait = std::chrono::milliseconds(50);

/// How long the drain waits after the most recent byte before calling the text
/// complete. A dump arrives in pipe-sized pieces, so stopping at the first
/// non-empty read would cut most of them off.
constexpr Micros AttributedOutputQuiet = std::chrono::milliseconds(5);

/// Bytes of one capture's own printing that are kept, per hit.
///
/// Bounded per capture rather than out of the shared window above, which is the
/// point of attributing it at all: a `dump()` at an early hit was silently
/// dropped by the front-erase as soon as the program said enough else to fill
/// that window, and for a compiler pass under any verbose flag that is every
/// run. Smaller than the run's bound because a dump recurs at every hit and the
/// same window is paid again each time.
constexpr size_t MaxAttributedOutput = 4096;

/// Hits kept for cycle detection, each a location and what was read there.
/// Enough for the longest period DetectCycle looks for to repeat several times
/// over.
constexpr size_t MaxTailEntries = MaxCyclePeriod * 8;

/// How long the wait loop blocks before it re-checks the run's deadline. A
/// program that is neither stopping nor hitting a tracepoint is only noticed
/// here, so the slice bounds how late a hang is reported.
constexpr Micros WaitSlice = std::chrono::milliseconds(200);



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

/// \p Text as a single line, bounded to \p MaxChars.
///
/// A refusal is reported as one clause of a sentence, and the reasons for one
/// include compiler diagnostics, which arrive as several lines. Kept whole
/// rather than reduced to the line that names the fault, the way a capture's
/// diagnostic is: the sentence the reason opens with is what says the recompile
/// is the thing that failed, and without it a diagnostic reads as the condition
/// itself having failed to evaluate.
std::string OneLine(StringRef Text, size_t MaxChars) {
  std::string Out;
  bool Pending = false;
  for (char C : Text) {
    if (isspace(static_cast<unsigned char>(C))) {
      Pending = !Out.empty();
      continue;
    }
    if (Pending) {
      Out += ' ';
      Pending = false;
    }
    Out += C;
  }
  if (Out.size() > MaxChars) {
    Out.resize(MaxChars);
    Out += "...";
  }
  return Out;
}

/// Locations of \p Bp that stand at code the program was built with.
///
/// A patched function's copy is compiled from the program's own source and its
/// line table points back at it, so a breakpoint on the original resolves into
/// the copy as well. That location is never armed -- the trap the copy already
/// holds is what reports these hits -- so counting it would report one
/// tracepoint as two places the program can be caught at.
uint32_t LocationsInTheProgram(Breakpoint &Bp) {
  uint32_t Count = 0;
  const size_t Locations = Bp.GetNumLocations();
  for (size_t I = 0; I < Locations; ++I) {
    lldb::BreakpointLocationSP Loc = Bp.GetLocationAtIndex(I);
    if (!Loc)
      continue;
    lldb::ModuleSP Module = Loc->GetAddress().GetModule();
    ObjectFile *Object = Module ? Module->GetObjectFile() : nullptr;
    if (Object && Object->GetType() == ObjectFile::eTypeJIT)
      continue;
    ++Count;
  }
  return Count;
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
  Described.Index = Frame.GetFrameIndex();
  // Collapsed here, where a name enters the engine, so that the terminal event,
  // a hit's frame, an event backtrace and the profile all report the same
  // spelling. Two instantiations of one function template fold together as a
  // consequence -- in a backtrace as a run of repeats, in the profile as one
  // entry -- which is what a reader asking where the program is wants of them.
  if (const char *Name = Frame.GetFunctionName())
    Described.Function = CollapseTemplateArguments(Name);
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
  //
  // Not once the injected code keeps the count: it applies the skip ahead of
  // the condition, so a hit that reaches here is one the program has already
  // let through, and applying it again would swallow the same hits twice.
  if (!CompiledIn && Hits <= Obs->SkipFirst)
    return false;

  // A gate opens on a state change rather than being re-tested at each hit,
  // which is what keeps it off the per-hit path entirely. It opens at the first
  // hit that is observed and not at the first that occurs, since a skipped hit
  // is one this observation was told not to see.
  if (!Opened) {
    Opened = true;
    for (ObservationSite *Dependent : Enables) {
      // Not for one whose work is compiled in: its location sits in a body the
      // redirect has made unreachable, and arming it there would write a trap
      // over the redirect itself. Its gate is what opens it.
      if (Dependent->Breakpoint && !Dependent->CompiledIn)
        Dependent->Breakpoint->SetEnabled(true);
      Engine->SetCompiledInGate(*Dependent, true);
    }
  }

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
  if (!GateReturns.Armed.Empty()) {
    if (Breakpoint && !CompiledIn)
      Breakpoint->SetEnabled(true);
    Engine->SetCompiledInGate(*this, true);
  }
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
    if (Breakpoint && !CompiledIn)
      Breakpoint->SetEnabled(false);
    Engine->SetCompiledInGate(*this, false);
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

CaptureCandidates ObservationEngine::CandidatesFor(StringRef Expr,
                                                   CaptureFailure Kind,
                                                   StackFrame *Frame) {
  std::vector<std::string> InScope;

  switch (Kind) {
  case CaptureFailure::Situational:
  case CaptureFailure::Malformed:
    // A value that is merely absent here, or an expression that does not parse,
    // is not a name anything could be suggested against.
    return {};

  case CaptureFailure::UnknownName: {
    if (!Frame)
      return {};
    // Globals excluded: the frame's own parameters and locals are what a caller
    // writing a bare name at a source location almost always meant, and the
    // file's globals are a much longer list that would crowd them out of a
    // bounded one. Synthetic variables likewise: a name a formatter invented is
    // not one the caller can be told to write.
    VariableList *Vars =
        Frame->GetVariableList(/*get_file_globals=*/false,
                               /*include_synthetic_vars=*/false,
                               /*error_ptr=*/nullptr);
    if (!Vars)
      return {};
    for (size_t I = 0, E = Vars->GetSize(); I != E; ++I)
      if (lldb::VariableSP Var = Vars->GetVariableAtIndex(I))
        if (StringRef Name = Var->GetName().GetStringRef();
            !Name.empty() && !IsInjectedName(Name))
          InScope.push_back(Name.str());
    break;
  }

  case CaptureFailure::UnknownMember: {
    // The part of the path before the member that failed. It resolved -- that
    // is why the compiler got as far as objecting to the member -- so resolving
    // it again reaches the same object, and its children are the type's fields.
    const size_t Split = Expr.find_last_of(".>");
    if (Split == StringRef::npos || !Frame)
      return {};
    StringRef Prefix = Expr.take_front(Split + 1).rtrim(".>-");
    if (Prefix.empty())
      return {};

    // Resolved through the frame's path parser directly rather than through
    // ResolveValueDWIM, which falls through to the expression evaluator when
    // the path tier fails. Compiling here would spend the run's wall clock on a
    // capture that has already been given up, which is the opposite of what
    // stopping a stable failure early is for.
    const bool AllowPointerPaths = CaptureOptions().AllowPointerPaths;
    if (!IsVariablePathEligible(Prefix, AllowPointerPaths))
      return {};

    lldb::VariableSP Unused;
    Status PathStatus;
    lldb::ValueObjectSP Base = Frame->GetValueForVariableExpressionPath(
        Prefix, lldb::eDynamicDontRunTarget,
        StackFrame::eExpressionPathOptionsAllowDirectIVarAccess |
            StackFrame::eExpressionPathOptionsDisallowGlobals,
        Unused, PathStatus,
        AllowPointerPaths ? lldb::eDILModeLegacy : lldb::eDILModeSimple);
    if (!Base || PathStatus.Fail() || Base->GetError().Fail())
      return {};

    // A pointer to an aggregate exposes the pointee's members as its own
    // children, so the fields are reached without a dereference step. Errors
    // are ignored: a type whose children cannot all be counted still has the
    // ones that were, and the names are all that is wanted.
    const uint32_t NumChildren = Base->GetNumChildrenIgnoringErrors();
    for (uint32_t I = 0; I != NumChildren; ++I)
      if (lldb::ValueObjectSP Child = Base->GetChildAtIndex(I))
        if (StringRef Name = Child->GetName().GetStringRef(); !Name.empty())
          InScope.push_back(Name.str());
    break;
  }
  }

  return RankCaptureCandidates(Expr, std::move(InScope));
}

Micros ObservationEngine::Remaining() const {
  const Micros Budget = std::chrono::seconds(m_plan.TimeoutSeconds);
  // Measured from the launch rather than from the call, so that reading a
  // program's debug info does not spend the budget for running it. Measured on a
  // 238 MB debug build: a misspelled function name took five minutes to answer,
  // and a plan asking for sixty seconds came back saying the program was still
  // running after sixty -- while it was still inside the dynamic loader, having
  // had none. A ceiling that the setup can exhaust does not bound what the caller
  // asked to bound, and it turns a slow answer into a false one.
  const Micros Spent =
      std::chrono::duration_cast<Micros>(Clock::now() - m_running_since);
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

  // The condition compiled into the program has already held -- that is why the
  // injected code trapped -- so it is not asked again. Paying for the answer
  // twice would be the smaller problem: an evaluation here that disagreed with
  // the one the program made would drop a hit the program has already counted
  // as one, and the two evaluators are not the same compiler.
  if (Obs.WhenExpr && Site.CompiledIn) {
    ++Site.ConditionTrue;
  } else if (Obs.WhenExpr) {
    // As for a capture: once a fixit has repaired the condition and the repair
    // has run, that is what gets evaluated for the rest of the run.
    StringRef Effective = Site.WhenFixedExpr.empty()
                              ? StringRef(*Obs.WhenExpr)
                              : StringRef(Site.WhenFixedExpr);

    const Clock::time_point Started = Clock::now();
    ValueResolution Resolved =
        ResolveValueDWIM(Effective, Frame, *m_target, Frame, CaptureOptions());
    Site.ConditionSpent +=
        std::chrono::duration_cast<Micros>(Clock::now() - Started);

    if (Resolved.Tier == ValueResolutionTier::Unresolved) {
      // Diagnosed at the first failure only. A condition is never turned off,
      // so it is evaluated at every hit of the run, and condensing a diagnostic
      // and gathering candidates at each one would repeat that work a hundred
      // times over to report it once.
      if (!Site.WhenDiagnosed) {
        Site.WhenDiagnosed = true;
        // Classified and described the way the capture loop does it, for the
        // same reason.
        const char *Raw = Resolved.Error.AsCString("");
        Site.WhenFailure = ClassifyCaptureFailure(Raw);
        Site.WhenFailureReason = DescribeCaptureFailure(Raw, Site.WhenFailure,
                                                        MaxFailureReasonChars);
        if (!Resolved.FixedExpression.empty() &&
            Resolved.FixedExpression != Effective)
          Site.WhenSuggestedFix = Resolved.FixedExpression;
        if (IsStableFailure(Site.WhenFailure))
          Site.WhenCandidates =
              CandidatesFor(Effective, Site.WhenFailure, Frame);
      }
    } else if (Site.WhenFixedExpr.empty() &&
               !Resolved.FixedExpression.empty() &&
               Resolved.FixedExpression != *Obs.WhenExpr) {
      Site.WhenFixedExpr = Resolved.FixedExpression;
    }

    // A condition is never turned off by cost control. Dropping it would start
    // recording the hits it exists to exclude, which is a different run rather
    // than a cheaper one. That holds for a condition that cannot resolve as
    // well: a condition assumed true would record every hit, and one assumed
    // false would record none, and neither is the run that was asked for. So a
    // stable failure here is reported rather than acted on -- which is why it
    // has to be reported somewhere a caller reads.
    std::optional<bool> Held;
    if (Resolved.Value) {
      Expected<bool> AsBool = Resolved.Value->GetValueAsBool();
      if (AsBool)
        Held = *AsBool;
      else if (!Site.WhenDiagnosed) {
        // A condition that resolved and is not a truth value -- `"when":
        // "Node"` where Node is a struct -- fails for a reason the resolution
        // never saw, so the diagnostic has to be taken from here or the report
        // has none. Left Situational, and the class set rather than left alone,
        // so the reason and the detail beside it cannot come from different
        // hits.
        Site.WhenDiagnosed = true;
        Site.WhenFailure = CaptureFailure::Situational;
        Site.WhenFailureReason = CondenseDiagnostic(
            toString(AsBool.takeError()), MaxFailureReasonChars);
      } else
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

  if (ReadState && Obs.OnReturn) {
    // Counted like any other capture, on the capture the caller named. `$return`
    // is the one expression that cannot be misspelled, so nothing in the report
    // has ever said anything about it -- it is left out of `capture_failures`
    // deliberately, since there is no name to correct -- and that left "read at
    // every hit" and "never readable at all" rendering as the same bare word.
    // A function returning void is the second case and looks perfectly healthy.
    //
    // Counted at every recorded hit rather than only where a value could be
    // read, so that a count of zero means the observation never fired and
    // nothing else.
    ObservationSite::CaptureState *ReturnState = nullptr;
    for (ObservationSite::CaptureState &Capture : Site.Captures)
      if (Capture.Expr == ReturnValueCapture) {
        ReturnState = &Capture;
        break;
      }
    if (ReturnState)
      ++ReturnState->Evaluations;

    bool Read = false;
    // At the return address the observed frame is already gone, so the value
    // the function produced is read from the ABI's result location rather than
    // from the frame. Captures naming the function's own locals cannot be read
    // here at all, which is what a return observation trades for the result.
    lldb::ThreadSP T = Frame ? Frame->GetThread() : lldb::ThreadSP();
    Process *P = T ? T->GetProcess().get() : nullptr;
    if (P && Site.ReturnType.IsValid()) {
      if (const lldb::ABISP &ABI = P->GetABI()) {
        if (lldb::ValueObjectSP Result = ABI->GetReturnValueObject(
                *T, Site.ReturnType, /*persistent=*/false)) {
          ValueObjectNode Node(Result);
          SerializeValueOptions SOpts;
          SOpts.MaxDepth = Obs.Depth;
          SOpts.MaxRenderedChars = MaxCaptureChars;
          SOpts.SawSummary = &m_saw_summary;
          SOpts.SawExpansion = &m_saw_expansion;
          json::Value V = SerializeValue(Node, SOpts);
          AggregatedValue Filed = AggregateKey(V);
          Read = !IsUnavailable(V);
          if (Read)
            m_aggregator.Record(Obs.Label, ReturnValueCapture, Filed.Key,
                                Filed.Document, Seq, Hit);
          // The tuple keeps it either way: it is what an emission mode and a
          // comparison read, and a hit whose return value could not be read is a
          // different hit from one where it could.
          Rendered += Filed.Key;
          Rendered += CaptureTupleSeparator;
          Values[ReturnValueCapture] = std::move(V);
        }
      }
    }
    if (ReturnState && !Read)
      ++ReturnState->Errors;
  }

  // Read once per hit rather than per capture, and only where a capture has
  // been turned off and so has something to re-check.
  std::optional<size_t> Modules;
  auto LoadedModules = [&]() -> size_t {
    if (!Modules)
      Modules = m_target ? m_target->GetImages().GetSize() : 0;
    return *Modules;
  };

  for (ObservationSite::CaptureState &Capture : Site.Captures) {
    if (!ReadState)
      break;
    if (Capture.Disabled) {
      // A name resolves nowhere until the library defining it is loaded, and
      // then it resolves. That is the one event that can make a settled failure
      // unsettled, so it is the one event that undoes the decision -- and
      // nothing else does, which is what keeps a capture that was given up from
      // being paid for again at every hit.
      if (LoadedModules() <= Capture.DisabledAtModules)
        continue;
      Capture.Disabled.reset();
    }

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
      lldb::ThreadSP T = Frame ? Frame->GetThread() : lldb::ThreadSP();
      Scope = T ? static_cast<ExecutionContextScope *>(T.get())
                : static_cast<ExecutionContextScope *>(m_target.get());
    }

    // A fixit already adopted is what gets evaluated. The evaluator applies one
    // itself and retries, so leaving the original in place would pay a failed
    // parse plus the retry at every remaining hit to reach the same expression.
    StringRef Effective = Capture.FixedExpr.empty()
                              ? StringRef(Capture.Expr)
                              : StringRef(Capture.FixedExpr);

    // The near side of the bracket that attributes printing to the expression
    // that printed it. Everything the program has written up to now is its own,
    // so it goes to the run's output before this expression can run; whatever
    // turns up afterwards arrived while this one expression was running.
    //
    // Per capture rather than once per stop, because two captures at one
    // tracepoint would otherwise both be credited with everything either of them
    // printed -- and a plan capturing two nodes' dumps at one location is the
    // shape this exists for.
    // Held for the whole bracket, so that the wait loop cannot drain the pipe in
    // the middle of one. The two run on different threads -- a synchronous
    // breakpoint callback is invoked on the private state thread while the wait
    // loop sits on the caller's -- and evaluating a capture resumes the process,
    // which is exactly what can wake that wait. Measured on the first printing
    // hit of a run: the wait loop took the nine bytes the capture had just
    // produced and filed them under the program's own output, at which point the
    // hit that caused them had nothing to show.
    //
    // It is also what makes the output buffers and the stderr offset safe to touch
    // from either thread at all.
    const Clock::time_point Started = Clock::now();
    std::lock_guard<std::recursive_mutex> Window(m_output_mutex);
    if (Capture.Prints)
      FlushInferiorOutput();
    DrainInferiorOutput();

    ValueResolution Resolved =
        ResolveValueDWIM(Effective, Obs.OnReturn ? nullptr : Frame, *m_target,
                         Scope, CaptureOptions());
    ++Capture.Evaluations;
    Capture.Tier = Resolved.Tier;

    // Read only for a capture that ran code. The variable-path tier is a
    // debug-info lookup and a memory read, so it executes nothing that could
    // print, and draining after it would credit this capture with output the
    // program produced on its own.
    InferiorOutput Printed;
    if (Resolved.Tier == ValueResolutionTier::Expression) {
      // The flush and the wait are spent where they can change an answer: an
      // expression whose value is void was run for its effect, and its effect is
      // what it printed. A capture that returned something is a capture whose
      // value is the answer, and paying a second evaluation plus a wait at every
      // hit of it to look for printing it probably did not do is exactly what
      // cost control exists to prevent.
      //
      // A capture already known to print keeps both however it resolves, since it
      // has answered the question this guess stands in for.
      const bool Effectful =
          Capture.Prints ||
          (Resolved.Value && !Resolved.Value->GetCompilerType().IsValid());
      if (Effectful)
        FlushInferiorOutput();
      DrainInferiorOutput(&Printed,
                          Effectful ? AttributedOutputWait : Micros::zero());
      if (!Printed.Empty())
        Capture.Prints = true;
    }

    // Measured over the whole bracket rather than over the resolution alone, so
    // that cost control sees what a printing capture actually costs: two flushes
    // and a wait at every hit is expensive whether or not the expense is the
    // expression's own.
    Capture.Spent += std::chrono::duration_cast<Micros>(Clock::now() - Started);

    if (Resolved.Tier == ValueResolutionTier::Unresolved) {
      ++Capture.Errors;
      // Classified against the whole diagnostic, and described from the line
      // that decided the class. A single expression draws more than one
      // objection -- a misspelled member reached with a dot through a pointer
      // draws both "is a pointer; did you mean to use '->'" and "no member
      // named", in that order -- so taking either the class or the wording from
      // the first line alone gets one of the two wrong.
      const char *Raw = Resolved.Error.AsCString("");
      Capture.Failure = ClassifyCaptureFailure(Raw);
      Capture.FailureReason =
          DescribeCaptureFailure(Raw, Capture.Failure, MaxFailureReasonChars);

      // A fixit offered against an expression that still did not resolve is a
      // suggestion, not a repair. Reported, never adopted: adopting it would
      // pin the capture to a spelling that has been shown not to work.
      if (!Resolved.FixedExpression.empty() &&
          Resolved.FixedExpression != Effective)
        Capture.SuggestedFix = Resolved.FixedExpression;

      // Gathered at the first failure worth explaining, and once. The names do
      // not change between hits of the same location, and walking for them at
      // every hit would cost the run what stopping early just saved it.
      if (!Capture.Diagnosed && IsStableFailure(Capture.Failure)) {
        Capture.Candidates = CandidatesFor(Effective, Capture.Failure,
                                           Obs.OnReturn ? nullptr : Frame);
        Capture.Diagnosed = true;
      }
    } else if (Capture.FixedExpr.empty() && !Resolved.FixedExpression.empty() &&
               Resolved.FixedExpression != Capture.Expr) {
      // The evaluator repaired the expression and the repair ran. Adopting it
      // is what makes the rest of the run cost one parse per hit instead of
      // two; the aggregate stays keyed on what the caller wrote.
      Capture.FixedExpr = Resolved.FixedExpression;
    }

    ValueObjectNode Node(Resolved.Value);
    SerializeValueOptions SOpts;
    SOpts.MaxDepth = Obs.Depth;
    SOpts.MaxRenderedChars = MaxCaptureChars;
    SOpts.ArtifactRef = formatv("$artifact#seq={0}", Seq).str();
    SOpts.SawSummary = &m_saw_summary;
    SOpts.SawExpansion = &m_saw_expansion;
    json::Value V = SerializeValue(Node, SOpts);

    // A call returning void completes and leaves a value object with no type
    // carrying a placeholder error, which serializes as a value that could not
    // be read. Reporting a call that ran as a failure costs more than saying
    // nothing: the caller re-spells a capture that was working, and stops
    // looking for what it printed -- which is the whole point of capturing a
    // dump.
    const bool ReturnedNothing =
        Resolved.Tier == ValueResolutionTier::Expression && Resolved.Value &&
        !Resolved.Value->GetCompilerType().IsValid();
    if (ReturnedNothing)
      V = json::Object{{"value", VoidValue}};

    // What this expression printed, on the expression rather than in a shared
    // buffer the caller has to guess at.
    //
    // For a call that returned nothing the text *is* the value, and stands where
    // one would: an expression run for its effect has its effect as its answer,
    // and in a codebase whose idiom is `N->dump()` that answer is the only legible
    // rendering of the node there is. The histogram then counts dumps, a
    // transition names the two of them it moved between, an outlier is the dump
    // seen once in four thousand hits, and a comparison of two runs is a
    // comparison of what each printed -- none of which a void marker can do.
    // `printed` stays where it is, holding the text raw and per stream: promotion
    // normalises and may shorten, and the bytes as written are what a caller
    // reading one hit closely is after.
    if (json::Value Written = Printed.Render(); Written != nullptr) {
      if (json::Object *Obj = V.getAsObject()) {
        if (ReturnedNothing) {
          if (PrintedValue AsValue = PrintedAsValue(Printed, MaxCaptureChars);
              !AsValue.Text.empty()) {
            (*Obj)["value"] = std::move(AsValue.Text);
            // Marked, because a reader who is not told will take the value for
            // something the expression returned -- and what a printer returned
            // is nothing, which is a different claim about the program.
            Obj->try_emplace(PrintedAsValueField, true);
            if (AsValue.Shortened)
              Obj->try_emplace("printed_value_elided",
                               formatv("the value is the first {0} characters of "
                                       "what was printed; \"printed\" has it whole",
                                       MaxCaptureChars)
                                   .str());
          }
        }
        Obj->try_emplace("printed", std::move(Written));
        if (Printed.Truncated)
          Obj->try_emplace("printed_elided",
                           formatv("only the last {0} bytes per stream",
                                   MaxAttributedOutput)
                               .str());
      }
    }

    AggregatedValue Filed = AggregateKey(V);

    // The aggregate sees every recorded hit, whatever the emission mode does
    // with the event. That invariant is the whole reason reducing the stream is
    // a saving rather than a loss.
    //
    // A value that could not be read is the exception, and it is not an exception
    // to that invariant: it is not a value the program took at this hit, it is an
    // error about the capture, and it is reported at top level in
    // `capture_failures` instead. Left in, one broken capture put an
    // `{"unavailable": ...}` into the histogram, onto both sides of a transition,
    // and into the outliers -- the array a caller is told to read first.
    if (!IsUnavailable(V))
      m_aggregator.Record(Obs.Label, Capture.Expr, Filed.Key, Filed.Document,
                          Seq, Hit);

    // The tuple keeps it either way. It is what an emission mode and a comparison
    // read, and a hit where a capture could not be read is a different hit from
    // one where it could -- `on_change` should say so, and two runs disagreeing
    // about it is exactly the kind of difference a comparison is for.
    Rendered += Filed.Key;
    Rendered += CaptureTupleSeparator;
    Values[Capture.Expr] = std::move(V);

    CaptureCostInput Cost;
    Cost.Tier = Capture.Tier;
    Cost.Spent = Capture.Spent;
    Cost.ObservedHits = Capture.Evaluations;
    Cost.TotalHits = Site.TotalHits();
    Cost.Remaining = Remaining();
    Cost.Errors = Capture.Errors;
    Cost.AtReturn = Obs.OnReturn;
    Cost.Failure = Capture.Failure;
    // Read from the breakpoint rather than from the pre-launch report, since a
    // location that arrived with a library counts: it is another program
    // counter this name may be in scope at.
    if (Site.Breakpoint)
      Cost.Locations = LocationsInTheProgram(*Site.Breakpoint);
    // The caller's own spelling, matching the key this capture is filed under.
    Cost.Expr = Capture.Expr;
    if (CaptureCostDecision Decision = AssessCaptureCost(Cost);
        Decision.Disable) {
      Capture.Disabled = std::move(Decision);
      Capture.DisabledAtModules = LoadedModules();
    }
  }

  FileHit(Site, Seq, std::move(Values), std::move(Rendered), Frame,
          /*AtAStop=*/true);
  return EndIfDue();
}

void ObservationEngine::FileHit(ObservationSite &Site, uint64_t Seq,
                                json::Object Values, std::string Rendered,
                                StackFrame *Frame, bool AtAStop) {
  const Observation &Obs = *Site.Obs;

  // The location and what was read there, so that a repetition means the program
  // is arriving at the same places holding the same values. The locations alone
  // repeat for any loop, wedged or not; the values are what separate them. A plan
  // with no captures contributes an empty value half and is judged on locations,
  // which is what the shortest useful plan for a hang looks like.
  {
    std::string Entry = Obs.Label;
    Entry += CycleEntryValueSeparator;
    Entry += Rendered;
    m_tail_entries.push_back(std::move(Entry));
  }
  if (m_tail_entries.size() > MaxTailEntries)
    m_tail_entries.erase(m_tail_entries.begin());

  // Kept per hit so that two runs of one plan can be compared hit by hit, which
  // is where the answer is: the hit at which they stopped agreeing. Bounded,
  // because a run of millions of hits would otherwise hold all of them, and the
  // bound is reported rather than assumed so that "they agreed" cannot mean "they
  // agreed as far as anyone looked".
  if (ObservationReport *Report = ReportFor(Obs.Label)) {
    if (Report->HitTuples.size() < MaxComparedHits)
      Report->HitTuples.push_back(Rendered);
    else
      ++Report->HitTuplesDropped;
  }

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
    json::Object Event{{"seq", static_cast<int64_t>(Seq)},
                       {"label", Obs.Label}};

    // Left off a hit the program recorded rather than stopped for. Nothing
    // watched that hit happen, so the only time to hand back would be the
    // moment its record was read -- which is one instant shared by the
    // thousands of hits read together, and would read as the program having
    // arrived at all of them at once.
    if (AtAStop)
      Event["t_ms"] =
          Round2(ToMs(std::chrono::duration_cast<Micros>(Clock::now() -
                                                         m_start)));

    // Hits of one observation are numbered and compared as a single sequence,
    // so on a program with more than one thread that sequence is an
    // interleaving. The thread is what lets a reader take it apart again.
    if (lldb::ThreadSP T = Frame ? Frame->GetThread() : lldb::ThreadSP())
      Event["tid"] = static_cast<int64_t>(T->GetID());
    if (Frame)
      Event["frame"] = DescribeFrame(*Frame).Function;
    else if (!Site.CompiledInFunction.empty())
      // The function whose copy recorded it. Not read from a frame, there having
      // been none, but known all the same: it is the function that was
      // recompiled to do this observation's work.
      Event["frame"] = Site.CompiledInFunction;
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
    Report.SourceNewerThanBinary = Resolved[I].SourceNewerThanBinary;
    Report.HasCondition = m_plan.Observations[I].WhenExpr.has_value();
    m_result.Observations.push_back(std::move(Report));
  }

  if (m_plan.Fast)
    ArrangeCompilingTracepointsIn();
  else
    for (ObservationReport &Report : m_result.Observations)
      Report.FallbackReason = "\"fast\" is off for this plan";

  return Error::success();
}

void ObservationEngine::ArrangeCompilingTracepointsIn() {
  // Nothing to arrange for a plan whose observations neither test a condition
  // nor read anything: a bare hit counter has nothing for the program to do that
  // the location's own trap does not already do more cheaply.
  if (none_of(m_plan.Observations, [](const Observation &Obs) {
        return Obs.WhenExpr.has_value() || !Obs.Capture.empty();
      }))
    return;

  const char *const Entry = "main";
  lldb::BreakpointSP Bp = m_target->CreateBreakpoint(
      /*containingModules=*/nullptr, /*containingSourceFiles=*/nullptr, Entry,
      lldb::eFunctionNameTypeFull, lldb::eLanguageTypeUnknown, /*offset=*/0,
      /*offset_is_insn_count=*/false, eLazyBoolNo, /*internal=*/true,
      /*request_hardware=*/false);
  if (Bp && Bp->GetNumLocations() != 0) {
    Bp->SetBreakpointKind("observe-compile-in");
    // Asynchronous, like every other internal stop this feature takes: a
    // callback that declines the stop there leaves the program resumed and
    // nothing reported, and this breakpoint is not one the caller asked for.
    Bp->SetCallback(CompileAtThisStop, std::make_shared<UntypedBaton>(this),
                    /*is_synchronous=*/false);
    m_compile_at = Bp->GetID();
    return;
  }

  if (Bp)
    m_target->RemoveBreakpointByID(Bp->GetID());

  // Said per observation rather than as a note, because it is the answer to
  // "which mode did this observation get" and that is where a caller reads it.
  for (ObservationReport &Report : m_result.Observations)
    Report.FallbackReason =
        "the program has no \"main\" to stop at, and a copy cannot be compiled "
        "before the dynamic loader has finished starting up";
}

bool ObservationEngine::CompileAtThisStop(void *Baton,
                                          StoppointCallbackContext *,
                                          lldb::user_id_t, lldb::user_id_t) {
  static_cast<ObservationEngine *>(Baton)->CompileTracepointsIntoProcess();
  return false;
}

std::optional<std::string>
ObservationEngine::WhyNotInProcess(const ObservationSite &Site) const {
  const Observation &Obs = *Site.Obs;

  if (Obs.OnReturn)
    return "an observation taken as the function returns is watched where its "
           "caller resumes, which is not in the function that would be "
           "recompiled";

  if (!Site.Enables.empty())
    return "another observation is enabled after this one, which happens at "
           "this one's first hit however its condition turned out -- and a hit "
           "whose condition was false is one the program does not stop for";

  if (m_plan.NoProgressSeconds)
    return "\"no_progress_seconds\" is measured over hits, and a hit the "
           "program does not stop for is one this run would not see in time to "
           "count as progress";

  if (!Site.Breakpoint)
    return "the tracepoint resolved to nothing that could be recompiled";

  const uint32_t Locations = LocationsInTheProgram(*Site.Breakpoint);
  if (Locations == 0)
    return "the tracepoint matched no code";
  if (Locations != 1)
    return formatv("the tracepoint matched {0} places, and a function is "
                   "recompiled one at a time",
                   Locations)
        .str();

  return std::nullopt;
}

std::optional<std::string>
ObservationEngine::WhyCapturesAreReadAtAStop(const ObservationSite &Site) const {
  const Observation &Obs = *Site.Obs;

  // Everything here has the same shape: something this hit produces cannot be
  // written into a record, so the debugger has to be standing in the frame. And
  // once it is standing there it reads every capture itself -- including the ones
  // no record could carry -- so recording them as well would be the same values
  // read twice.
  if (Obs.Backtrace != 0)
    return "a backtrace is the stack the hit happened on, which only exists "
           "while the program is held still at it";

  if (Obs.OnlyHit)
    return "\"only_hit\" names one of the hits this observation records, and "
           "which hit that is is decided where the hits are counted";

  // The gate byte the program reads is one byte for all of them, so it can only
  // say that some thread is inside the gating function. Which is enough while the
  // hit still stops: the gate keeps the callback from running at all when nobody
  // is inside, and the callback then answers the real question. Nothing answers
  // it for a hit nobody sees.
  if (Obs.CalledFrom)
    return formatv("\"called_from\" is whether a frame of \"{0}\" is below this "
                   "hit on the same thread, which is a question about a stack "
                   "and not about a byte the program can read",
                   *Obs.CalledFrom)
        .str();

  for (const std::string &Expr : Obs.Capture)
    if (Expr == ReturnValueCapture)
      return formatv("\"{0}\" is read from where the ABI leaves a result, "
                     "which is a register rather than anything the function's "
                     "own code can name",
                     ReturnValueCapture)
          .str();

  return std::nullopt;
}

void ObservationEngine::CompileTracepointsIntoProcess() {
  // Asked once. A refusal at this stop is a refusal for a reason the rest of
  // the run cannot change, and a second attempt would recompile every function
  // that succeeded.
  if (m_compile_attempted || !m_target)
    return;
  m_compile_attempted = true;
  if (lldb::BreakpointSP Bp = m_target->GetBreakpointByID(m_compile_at))
    Bp->SetEnabled(false);

  FunctionPatchManager &Patches = m_target->GetFunctionPatchManager();
  std::vector<std::string> Recompiled;

  // Decided for every tracepoint before any of them is compiled, so that several
  // tracepoints in one function cost one compile of it rather than one each. A
  // compile runs clang over the whole body and JITs the result into the program,
  // and each one retires a copy the target keeps for its own lifetime.
  struct Pending {
    ObservationSite *Site;
    PatchRequest Request;
    bool Records;
    std::string FunctionName;
    bool WasEnabled;
  };
  std::vector<Pending> Wanted;

  for (std::unique_ptr<ObservationSite> &Owned : m_sites) {
    ObservationSite &Site = *Owned;
    ObservationReport &Report = m_result.Observations[Site.Index];
    const Observation &Obs = *Site.Obs;
    if (!Obs.WhenExpr && Obs.Capture.empty())
      continue;
    if (std::optional<std::string> Why = WhyNotInProcess(Site)) {
      Report.FallbackReason = std::move(*Why);
      continue;
    }

    // Captures go into the program exactly when that lets the site stop for
    // nothing at all. While it still traps at every hit the guards let through,
    // the debugger is standing in the frame and reads every capture there --
    // anything a record can carry and everything it cannot -- so recording them
    // as well would be the same values read twice.
    std::optional<std::string> CapturesReadAtAStop;
    if (!Obs.Capture.empty())
      CapturesReadAtAStop = WhyCapturesAreReadAtAStop(Site);
    const bool Records = !Obs.Capture.empty() && !CapturesReadAtAStop;

    // Which can leave nothing for the program to do: a site with no condition
    // whose captures have to be read at a stop stops at every hit, which is what
    // it would do with this feature switched off.
    if (!Obs.WhenExpr && !Records) {
      Report.FallbackReason = *CapturesReadAtAStop;
      continue;
    }

    lldb::BreakpointLocationSP Loc = Site.Breakpoint->GetLocationAtIndex(0);
    SymbolContext SC;
    if (Loc)
      Loc->GetAddress().CalculateSymbolContext(
          &SC, lldb::eSymbolContextFunction | lldb::eSymbolContextLineEntry);
    if (!SC.function || SC.line_entry.line == 0) {
      Report.FallbackReason =
          "no debug info describes a function and a line at the tracepoint";
      continue;
    }

    PatchRequest Request;
    // The entry, whatever line the tracepoint is on: the whole function is
    // recompiled and reached through a redirect written over its first
    // instructions.
    Request.FunctionEntry =
        SC.function->GetAddress().GetLoadAddress(m_target.get());
    Request.Line = SC.line_entry.line;
    Request.Condition = Obs.WhenExpr;
    if (Records)
      Request.Captures = Obs.Capture;

    // Only what the skip has left. The hits taken before this moment were
    // counted at a stop and skipped there, and the injected code's counter
    // starts from zero, so passing the whole of `skip_first` would swallow the
    // same hits a second time.
    Request.SkipFirst =
        Obs.SkipFirst > Site.Hits
            ? Obs.SkipFirst - static_cast<uint32_t>(Site.Hits)
            : 0;

    // `only_hit` is deliberately not passed. The injected code would compare it
    // against every hit the location took, while the report numbers the hits
    // this observation recorded -- and with a condition in play those are
    // different numbers, so the two would select different hits. Applied at the
    // stop instead, which costs a trap per hit whose condition held and picks
    // the hit the aggregate names.
    Request.Gated = Obs.CalledFrom.has_value() || Obs.EnabledAfter.has_value();

    // A site that has written its values into the ring has already said
    // everything the hit had to say, so stopping would cost exactly what the
    // recording saved.
    Request.WantStop = !Records;
    Request.OnTrap = TracepointHit;
    Request.Baton = std::make_shared<UntypedBaton>(&Site);
    Request.HitsCarriedBy = Site.Breakpoint->GetID();

    // Marked before the install, because announcing the copy is part of
    // installing it, and that is the moment this breakpoint acquires a location
    // inside the copy which must not be armed.
    Site.Breakpoint->SetHitsComeFromCompiledCode();

    // And its own trap taken down before the redirect is written, for two
    // reasons that both end in a corrupted program. A tracepoint on a function
    // resolves to the end of its prologue, which for a small function is inside
    // the first four instructions -- the ones the redirect is written over --
    // so the patch is refused outright while a trap sits there. And a software
    // breakpoint holds the byte it displaced and puts it back when it is
    // removed, which after the redirect would restore an old instruction over
    // part of it.
    //
    // Nothing is lost by taking it down: the body it sits in stops being
    // reached, and the trap in the copy is what reports this breakpoint's hits
    // from here on. It stays down for the rest of the run -- see the gate.
    const bool WasEnabled = Site.Breakpoint->IsEnabled();
    Site.Breakpoint->SetEnabled(false);

    Wanted.push_back(Pending{&Site, std::move(Request), Records,
                             SC.function->GetName().GetString(), WasEnabled});
  }

  std::vector<PatchRequest> Requests;
  Requests.reserve(Wanted.size());
  for (const Pending &P : Wanted)
    Requests.push_back(P.Request);

  const std::vector<FunctionPatchManager::InstallOutcome> Outcomes =
      Patches.Install(Requests);

  for (size_t I = 0; I < Wanted.size(); ++I) {
    const Pending &P = Wanted[I];
    ObservationSite &Site = *P.Site;
    ObservationReport &Report = m_result.Observations[Site.Index];
    const bool Records = P.Records;

    if (!Outcomes[I].SiteID) {
      Site.Breakpoint->SetHitsComeFromCompiledCode(false);
      Site.Breakpoint->SetEnabled(P.WasEnabled);
      Report.FallbackReason =
          OneLine(Outcomes[I].Refusal, MaxFailureReasonChars);
      continue;
    }

    Site.CompiledIn = *Outcomes[I].SiteID;
    Site.RecordsWithoutStopping = Records;
    Site.CompiledInFunction = P.FunctionName;
    Site.HitsBeforeCompile = Site.Hits;
    Site.ConditionTrueBeforeCompile = Site.ConditionTrue;
    Report.InProcess = true;
    if (Records) {
      m_recorded_without_stopping = true;
      // Set here rather than where a value arrives, so that a capture on an
      // observation whose code never ran still reports the mode it was given
      // rather than the mode it would have had.
      for (ObservationSite::CaptureState &Capture : Site.Captures)
        Capture.InProcess = true;
      Site.Partial.clear();
    }

    // A gated site starts closed, and the debugger may already have opened this
    // observation. The gate byte mirrors the tracepoint's own enabled bit,
    // which is the debugger's answer to whether this observation should be
    // doing anything at all.
    if (P.Request.Gated)
      Patches.SetGate(*Outcomes[I].SiteID, P.WasEnabled);

    // The function, not the observation: two tracepoints in one function are
    // one recompile, and it is the recompile the note is about.
    if (!llvm::is_contained(Recompiled, Site.CompiledInFunction))
      Recompiled.push_back(Site.CompiledInFunction);
  }

  if (Recompiled.empty())
    return;

  // One note for the run, because the program under test is running unoptimized
  // copies of these functions: slower than what it was built as, and where the
  // original relied on what the optimizer did, not always the same code. That
  // is a property of the run rather than of any one observation.
  //
  // Bounded and the rest counted, for the same reason every other list here is:
  // a plan of twenty observations would otherwise put twenty names in one
  // sentence.
  constexpr size_t MaxNamed = 3;
  std::string Names;
  for (size_t I = 0; I < Recompiled.size() && I < MaxNamed; ++I) {
    if (I != 0)
      Names += ", ";
    Names += formatv("\"{0}\"", Recompiled[I]).str();
  }
  if (Recompiled.size() > MaxNamed)
    Names += formatv(" and {0} more", Recompiled.size() - MaxNamed).str();
  m_result.Notes.push_back(
      formatv(
          "this run recompiled {0} without optimization and pointed the "
          "program at the {1}, so that the tracepoints on {2} could run "
          "without stopping it. The program's own timing is therefore not "
          "what it was built to be, and code that relied on what the "
          "optimizer did can behave differently; \"fast\": false leaves the "
          "program as it was built, at a stop per hit.",
          Names, Recompiled.size() == 1 ? "copy" : "copies",
          Recompiled.size() == 1 ? "it" : "them")
          .str());

  // Said once for the run, because it is a property of how a hit was observed
  // rather than of any one observation: a hit the program recorded for itself was
  // never current anywhere, so there was no thread to name it on and no moment at
  // which anything looked at the clock. Every other field of such an event is what
  // it would have been at a stop.
  if (m_recorded_without_stopping)
    m_result.Notes.push_back(
        "the hits whose captures are marked \"in_process\" were recorded by the "
        "program without stopping it, so their events carry no \"tid\" and no "
        "\"t_ms\": nothing watched them happen. Their values, their order and "
        "their counts are what a stop would have reported.");
}

void ObservationEngine::SetCompiledInGate(const ObservationSite &Site,
                                          bool Open) {
  if (Site.CompiledIn && m_target)
    m_target->GetFunctionPatchManager().SetGate(*Site.CompiledIn, Open);
}

void ObservationEngine::TakeWhatTheProgramRecorded(bool RunHasEnded) {
  if (!m_target ||
      none_of(m_sites, [](const std::unique_ptr<ObservationSite> &Site) {
        return Site->CompiledIn.has_value();
      }))
    return;

  Expected<PatchDrainResult> Drained =
      m_target->GetFunctionPatchManager().Drain();
  if (!Drained) {
    // Loud, because the alternative is an observation that reports the hits it
    // was stopped for -- none, for a condition that never held -- as the hits
    // the program took.
    //
    // Nothing held from an earlier read is stranded by returning here. A drain
    // reports a failure rather than what it has only when it has nothing at all,
    // which for a run that had read anything before cannot be true.
    m_result.Notes.push_back(
        formatv("what the compiled-in tracepoints recorded could not be read "
                "back out of the program: {0}. The observations reported as "
                "\"in-process\" undercount their hits by however many the "
                "program did not stop for, and are missing the values it "
                "recorded at them.",
                StringRef(toString(Drained.takeError())).rtrim())
            .str());
    return;
  }

  std::vector<std::string> Unread;
  for (std::unique_ptr<ObservationSite> &Site : m_sites) {
    if (!Site->CompiledIn)
      continue;
    auto Counted = Drained->Counters.find(*Site->CompiledIn);
    if (Counted == Drained->Counters.end()) {
      Unread.push_back(Site->Obs->Label);
      continue;
    }
    Site->CompiledInHits = Counted->second.Hits;
    Site->CompiledInConditionTrue = Counted->second.CondTrue;
  }

  // Each value against the hit that recorded it. The site's own counter numbered
  // that hit, and the number travels in the record, because two threads inside
  // one site write theirs interleaved: joined by arrival order instead, a hit
  // would be built out of one thread's first value and another's second, and the
  // tuple that results is a hit the program never had.
  llvm::DenseMap<uint32_t, ObservationSite *> BySite;
  for (std::unique_ptr<ObservationSite> &Site : m_sites)
    if (Site->CompiledIn && Site->RecordsWithoutStopping)
      BySite[*Site->CompiledIn] = Site.get();

  for (CapturedValue &Recorded : Drained->Values) {
    ObservationSite *Site = BySite.lookup(Recorded.SiteID);
    if (!Site || Recorded.Capture >= Site->Captures.size()) {
      // A value nobody can put anywhere: written by a site this run no longer
      // knows, or naming a capture the observation does not have. Counted rather
      // than passed over, since it is a value the program produced and the
      // report does not hold.
      ++m_records_lost;
      continue;
    }

    auto Held = llvm::find_if(Site->Partial,
                              [&](const ObservationSite::PartialHit &Hit) {
                                return Hit.Hit == Recorded.Hit;
                              });
    if (Held == Site->Partial.end()) {
      ObservationSite::PartialHit Fresh;
      Fresh.Hit = Recorded.Hit;
      Fresh.Values.resize(Site->Captures.size());
      Site->Partial.push_back(std::move(Fresh));
      Held = std::prev(Site->Partial.end());
    }
    // Counted only for a slot that was empty, so that a record read twice cannot
    // make a hit look complete when it is not.
    if (!Held->Values[Recorded.Capture])
      ++Held->Filled;
    Held->Values[Recorded.Capture] = std::move(Recorded.Value);
  }

  for (std::unique_ptr<ObservationSite> &Owned : m_sites) {
    ObservationSite &Site = *Owned;
    if (!Site.RecordsWithoutStopping)
      continue;

    // A hit is filed once every capture of it has arrived. A thread held still
    // partway through one has written some of its values and not the rest, and
    // while the program is still running the rest is likelier to arrive than not
    // -- so the hit waits, in the position it arrived in, rather than being
    // reported a value short.
    //
    // Once the program has stopped for good, what is there is all there will be.
    // Bounded either way: a hit whose remaining values the ring overwrote would
    // otherwise be held for the life of the run, and the queue holding it would
    // grow by one for every such hit.
    const bool ReleaseEverything =
        RunHasEnded || Site.Partial.size() > MaxPartialHits;
    llvm::erase_if(Site.Partial, [&](ObservationSite::PartialHit &Hit) {
      if (Hit.Filled < Hit.Values.size() && !ReleaseEverything)
        return false;
      RecordCompiledInHit(Site, Hit.Values);
      return true;
    });
  }

  m_records_lost += Drained->Lost;
  if (!RunHasEnded)
    return;

  // Said once, at the end. Every stop of the run reads what the program has
  // recorded since the last one, and a note pushed at each of them would repeat
  // itself as many times as the run stopped.
  if (!Unread.empty())
    m_result.Notes.push_back(
        formatv("the program never reported the hits it counted for {0}, so "
                "those observations undercount: what is reported is the hits "
                "they were stopped for.",
                join(Unread, ", "))
            .str());

  if (m_records_lost != 0)
    m_result.Notes.push_back(
        formatv("{0} values the compiled-in tracepoints recorded never reached "
                "this report, because they were overwritten before they could "
                "be read. The hits they belonged to are still counted; what was "
                "captured at them is missing from \"aggregate\".",
                m_records_lost)
            .str());

  // Only where the values are the ones at risk: a run that never fills the ring
  // never raises a drain trap, so nothing but the stop on the way out reads what
  // it recorded -- and where that stop could not be set, the tail of the run is
  // missing rather than empty.
  if (m_recorded_without_stopping) {
    StringRef Refusal =
        m_target->GetFunctionPatchManager().GetTailDrainRefusal();
    if (!Refusal.empty())
      m_result.Notes.push_back(
          formatv("the values recorded after this run's last stop may be "
                  "missing: {0}. What is reported is what had already been read.",
                  Refusal)
              .str());
  }
}

void ObservationEngine::RecordCompiledInHit(
    ObservationSite &Site, llvm::ArrayRef<lldb::ValueObjectSP> Values) {
  const Observation &Obs = *Site.Obs;

  // Counted here for the same reason the stopping path counts it there: it is
  // what numbers the hit, and what an emission mode compares against.
  ++Site.Recorded;

  // Ordered as the records were written, which for hits of one site is the order
  // the program took them in. The number itself is assigned at the read rather
  // than at the hit, so a hit recorded without a stop and one stopped for later
  // in the run can appear in the stream out of order -- there being no clock in
  // the record to put them back with.
  const uint64_t Seq = ++m_seq;
  const uint64_t Hit = static_cast<uint64_t>(Obs.SkipFirst) + Site.Recorded;

  json::Object Rendered;
  std::string Tuple;
  for (size_t I = 0; I < Site.Captures.size(); ++I) {
    ObservationSite::CaptureState &Capture = Site.Captures[I];
    ++Capture.Evaluations;

    const lldb::ValueObjectSP Value = I < Values.size() ? Values[I]
                                                        : lldb::ValueObjectSP();
    if (!Value) {
      // The record carrying it never reached the debugger. Counted as an error on
      // the capture, which is where a reader looks to find out whether a value
      // was ever read, and given a reason there, since a capture that failed at
      // every hit earns an entry in `capture_failures` and an entry with nothing
      // in the reason field explains nothing.
      //
      // Situational rather than stable: the value was recorded, and what went
      // wrong is a property of this hit and not of the expression, so the next
      // hit is a fresh question.
      ++Capture.Errors;
      Capture.Failure = CaptureFailure::Situational;
      Capture.FailureReason =
          "the program recorded this value, and the record was overwritten "
          "before the debugger could read it";
    }

    ValueObjectNode Node(Value);
    SerializeValueOptions SOpts;
    SOpts.MaxDepth = Obs.Depth;
    SOpts.MaxRenderedChars = MaxCaptureChars;
    SOpts.ArtifactRef = formatv("$artifact#seq={0}", Seq).str();
    SOpts.SawSummary = &m_saw_summary;
    SOpts.SawExpansion = &m_saw_expansion;
    json::Value V = SerializeValue(Node, SOpts);

    AggregatedValue Filed = AggregateKey(V);
    if (!IsUnavailable(V))
      m_aggregator.Record(Obs.Label, Capture.Expr, Filed.Key, Filed.Document,
                          Seq, Hit);
    Tuple += Filed.Key;
    Tuple += CaptureTupleSeparator;
    Rendered[Capture.Expr] = std::move(V);
  }

  FileHit(Site, Seq, std::move(Rendered), std::move(Tuple), /*Frame=*/nullptr,
          /*AtAStop=*/false);
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

  // Standard error is given a file of this run's own rather than sharing the
  // program's terminal with standard output. Two things follow, and both are the
  // point.
  //
  // The streams become separable. Launched on one pty they arrive interleaved
  // with no marker and no relative order -- two buffers drained in turn know
  // nothing about which write came first -- so a `dump()` on stderr could not be
  // told from the compiler's own result on stdout, which is the one distinction a
  // reader of a compiler's output needs.
  //
  // And a file is read here synchronously at an offset, while a pty is read by a
  // thread of LLDB's own. What an expression wrote is therefore readable the
  // instant that expression returns, rather than whenever that thread next
  // happens to run: measured on a capture calling a printer, the pty gave up
  // nothing at all at the first hit and the text surfaced during the second.
  //
  // Only stderr moves. Redirecting stdout as well would take it off a terminal
  // and make it fully buffered, so a program that hung would be reported with its
  // last line still inside it -- and that line is the whole reason a run reports
  // the program's output. stderr is unbuffered whatever it is connected to, so it
  // loses nothing by moving.
  if (m_plan.CaptureInferiorOutput) {
    SmallString<128> Path;
    if (!sys::fs::createTemporaryFile("lldb-observe-stderr", "txt", Path)) {
      m_stderr_path = std::string(Path);
      Info.AppendOpenFileAction(STDERR_FILENO, FileSpec(m_stderr_path),
                                /*read=*/false, /*write=*/true);
      if (Expected<sys::fs::file_t> Reader =
              sys::fs::openNativeFileForRead(m_stderr_path))
        m_stderr_reader = *Reader;
      else
        consumeError(Reader.takeError());
    }
    // A failure to make the file is not a failure of the run: stderr stays on the
    // terminal, where it is still reported and still readable, just not
    // separately and not promptly. Silent, because a caller can do nothing about
    // a temp directory.
  }

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

    // Read here as well as at the end of the run, so that a run long enough to
    // be cut short by its ceiling reports the hits it took rather than the ones
    // it happened to have stopped for.
    TakeWhatTheProgramRecorded(/*RunHasEnded=*/false);

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

ObservationReport *ObservationEngine::ReportFor(StringRef Label) {
  for (ObservationReport &Report : m_result.Observations)
    if (Report.Label == Label)
      return &Report;
  return nullptr;
}

void ObservationEngine::DrainInferiorOutput(InferiorOutput *Into, Micros Wait) {
  if (!m_plan.CaptureInferiorOutput)
    return;
  lldb::ProcessSP P = m_target->GetProcessSP();
  if (!P)
    return;

  // Recursive because a capture's bracket already holds it across this call, and
  // the point of that is to keep the other thread out for the whole window rather
  // than for each read.
  std::lock_guard<std::recursive_mutex> Reading(m_output_mutex);

  InferiorOutput &Sink = Into ? *Into : m_result.Output;
  const size_t Max = Into ? MaxAttributedOutput : MaxInferiorOutput;

  auto ReadWhatIsThere = [&] {
    size_t Total = 0;
    char Buffer[1024];

    // stderr first, out of this run's own file. Read at an offset, so it needs no
    // waiting and cannot be a hit late.
    while (m_stderr_reader != sys::fs::kInvalidFile) {
      Expected<size_t> Read = sys::fs::readNativeFileSlice(
          m_stderr_reader, MutableArrayRef<char>(Buffer, sizeof(Buffer)),
          m_stderr_read);
      if (!Read) {
        consumeError(Read.takeError());
        break;
      }
      if (*Read == 0)
        break;
      m_stderr_read += *Read;
      Sink.Append(/*Stderr=*/true, StringRef(Buffer, *Read), Max);
      Total += *Read;
      if (*Read < sizeof(Buffer))
        break;
    }

    // Then whatever the process has handed over. Both of its streams are read to
    // exhaustion whatever this is draining into: a capture's window is a window
    // on *when*, not on which pipe, and leaving stdout unread while attributing
    // stderr would deadlock a chatty program at its next write. Draining as the
    // run goes rather than only at the end is the same rule.
    //
    // `GetSTDERR` is still asked, and returns nothing when the redirect above
    // took effect. It is what covers the run where it did not.
    for (bool Stderr : {false, true}) {
      while (true) {
        Status Err;
        const size_t Read = Stderr ? P->GetSTDERR(Buffer, sizeof(Buffer), Err)
                                   : P->GetSTDOUT(Buffer, sizeof(Buffer), Err);
        if (Read == 0)
          break;
        Sink.Append(Stderr, StringRef(Buffer, Read), Max);
        Total += Read;
        if (Read < sizeof(Buffer))
          break;
      }
    }
    return Total;
  };

  if (Wait == Micros::zero()) {
    ReadWhatIsThere();
    return;
  }

  // Waiting for a quiet period rather than for the first byte, because a dump
  // arrives in pipe-sized pieces and the reader thread hands them over as it
  // gets them.
  const Clock::time_point Deadline = Clock::now() + Wait;
  Clock::time_point Latest = Clock::now();
  bool Seen = false;
  while (Clock::now() < Deadline) {
    if (ReadWhatIsThere() != 0) {
      Seen = true;
      Latest = Clock::now();
      continue;
    }
    if (Seen && Clock::now() - Latest >= AttributedOutputQuiet)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void ObservationEngine::FlushInferiorOutput() {
  // `llvm::errs()` is unbuffered and `outs()` is not, so a capture that dumps
  // through the buffered stream leaves nothing to read at the hit that produced
  // it: the text surfaces whenever the buffer next fills, attributed to a hit
  // that had nothing to do with it. Measured on an agent working around exactly
  // this -- it appended `(void)fflush(0)` to two captures by hand and tried a
  // third spelling three ways -- which is the caller doing the run's job for it.
  //
  // `fflush(0)` rather than a named stream: it flushes every open output stream,
  // so it covers a program's own `FILE *` as well as the C++ streams, and it
  // needs no symbol the program might not have beyond `fflush` itself.
  ValueResolutionOptions Opts = CaptureOptions();
  ValueResolution Flushed = ResolveValueDWIM("(void)fflush(0)", nullptr, *m_target,
                                             m_target.get(), Opts);
  // Nothing is reported. A program without a C library to call has buffered
  // output that arrives late, which is the state this exists to improve on, and
  // saying so once per hit would fill the response with a fact about the program
  // rather than about the capture.
  (void)Flushed;
}

void ObservationEngine::CollectTerminalEvent(Outcome Result) {
  TerminalEvent &Terminal = m_result.Terminal;
  lldb::ProcessSP P = m_target->GetProcessSP();

  // Both terms of the wall clock, said in the sentence the run is already
  // generating rather than in a field a bound might suppress.
  const std::string Clocks = DescribeWallClock(
      ToMs(std::chrono::duration_cast<Micros>(Clock::now() - m_running_since)),
      m_result.SetupMs);

  switch (Result) {
  case Outcome::Exited:
    Terminal.Description = "the program ran to completion";
    break;
  case Outcome::Crashed:
    Terminal.Description = "the program stopped abnormally";
    break;
  case Outcome::TimedOut:
    Terminal.Description =
        formatv("the program was still running after {0}s and was stopped{1}",
                m_plan.TimeoutSeconds, Clocks)
            .str();
    break;
  case Outcome::NoProgress:
    // "Hit", not "emitted": progress is any arrival at a tracepoint, so a run
    // whose condition never holds keeps making progress and never ends here.
    // Saying "emitted" would send a reader to change an emission mode that had
    // nothing to do with it.
    Terminal.Description =
        formatv("no tracepoint was hit for {0}s and the program was stopped{1}",
                m_plan.NoProgressSeconds.value_or(0), Clocks)
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

  // A halt is delivered to the process and reported on its first thread, so a run
  // this engine stopped named the thread that was *waiting for* the answer:
  // measured on a program whose main thread was parked in `pthread_join` while a
  // worker spun, the whole terminal event described `main` in the join, 339 of the
  // response's 1,100 characters spent saying the program was waiting for itself.
  // The profile already knows which thread was working, and it has finished
  // sampling by the time this runs.
  //
  // A crash needs none of this and must not get it: the faulting thread is the
  // answer, and it is the thread the stop is already reported on. `BusiestThread`
  // returns 0 for a run that was never sampled, so a short hang falls through to
  // exactly the behaviour it had before.
  lldb::ThreadSP T;
  if (IsCeilingStop(Result))
    if (const lldb::tid_t Busiest = m_profile.BusiestThread())
      T = P->GetThreadList().FindThreadByID(Busiest);
  if (!T)
    T = P->GetThreadList().GetSelectedThread();
  if (!T)
    T = P->GetThreadList().GetThreadAtIndex(0);
  if (!T)
    return;

  Terminal.Tid = T->GetID();
  Terminal.ThreadCount = static_cast<uint32_t>(P->GetThreadList().GetSize());

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

  uint32_t Dropped = 0;
  Terminal.Frames = RankFrames(Raw, &Dropped);
  Terminal.FramesOmitted = Dropped;
  if (Terminal.Frames.size() > MaxTerminalFrames) {
    // Counted in raw frames rather than in entries, so that the total the report
    // gives is the sum of what it shows and what it left out. An entry standing
    // for a run of eight recursive frames is eight of the total, and subtracting
    // one for it would leave a reader unable to make the numbers meet.
    for (size_t I = MaxTerminalFrames, E = Terminal.Frames.size(); I < E; ++I)
      Terminal.FramesOmitted += Terminal.Frames[I].Repeats;
    Terminal.Frames.resize(MaxTerminalFrames);
  }
  if (!Terminal.Frames.empty()) {
    Terminal.Function = Terminal.Frames.front().Function;
    Terminal.File = Terminal.Frames.front().File;
    Terminal.Line = Terminal.Frames.front().Line;
    Terminal.FrameIndex = Terminal.Frames.front().Index;
  }

  // Read out of the frame the report names, not out of frame zero. Ranking drops
  // the frames that resolved no source, so the two agree only when the innermost
  // raw frame had source -- and a fault inside a library does not: a null
  // dereference reached through `strlen` stops in `_platform_strlen`, which has
  // no variables and no source, while the frame the report names has both.
  //
  // Read from the wrong frame the fields do not disagree visibly, they vanish:
  // the response still names a function, a file and a line, and drops the locals
  // and the listing that would let a reader check them.
  lldb::StackFrameSP Frame = T->GetStackFrameAtIndex(Terminal.FrameIndex);
  if (!Frame)
    return;

  // The dynamic type is worth a call into the inferior exactly once, and this
  // is the once: a terminal event happens one time per run.
  if (VariableList *Locals = Frame->GetVariableList(
          /*get_file_globals=*/false, /*include_synthetic_vars=*/true,
          /*error_ptr=*/nullptr)) {
    // Read and ranked before anything is rendered, because the order the budget
    // is spent in decides which locals the response holds. See \ref
    // TerminalLocalRank.
    struct Local {
      llvm::StringRef Name;
      lldb::ValueObjectSP Value;
      unsigned Rank = 0;
    };
    std::vector<Local> Ordered;
    Ordered.reserve(std::min(Locals->GetSize(), MaxTerminalLocals));
    // Names the injected code introduced, counted so that the marker below does
    // not report them as locals of the program that went unread. A caller told
    // that something was withheld raises a budget, and this is not what any
    // budget was spent on.
    size_t Injected = 0;
    // The bound counts the locals that are going to be reported rather than the
    // ones walked to find them, so a frame carrying names nothing can be told to
    // do anything with does not spend the bound on them.
    for (size_t I = 0, E = Locals->GetSize();
         I < E && Ordered.size() < MaxTerminalLocals; ++I) {
      lldb::VariableSP Var = Locals->GetVariableAtIndex(I);
      if (!Var)
        continue;
      // Asked of the variable rather than of the value, so that a name this
      // skips costs no call into the inferior to read what it holds.
      //
      // A local with no name cannot be named in a capture either, so reporting
      // it costs a reader an entry keyed on the empty string and offers nothing
      // to do with it. Nor can one the injected code introduced: a frame of a
      // patched function is still the program's frame, and these are the
      // debugger's own working locals standing in it.
      const llvm::StringRef Declared = Var->GetName().GetStringRef();
      if (Declared.empty())
        continue;
      if (IsInjectedName(Declared)) {
        ++Injected;
        continue;
      }
      lldb::ValueObjectSP Value = Frame->GetValueObjectForFrameVariable(
          Var, lldb::eDynamicCanRunTarget);
      if (!Value)
        continue;
      const llvm::StringRef Name = Value->GetName().GetStringRef();
      if (Name.empty())
        continue;
      // `IsScalarType` computes no synthetic children, so asking it of every
      // local costs nothing the ranking would otherwise avoid.
      Ordered.push_back(
          {Name, Value,
           TerminalLocalRank(Name, Value->GetCompilerType().IsScalarType(),
                             Var->GetScope() ==
                                 lldb::eValueTypeVariableArgument)});
    }

    // Stable, so that within one rank the compiler's own order stands: among a
    // function's body locals, declaration order is as good an answer as any and
    // it does not move between runs.
    llvm::stable_sort(Ordered, [](const Local &LHS, const Local &RHS) {
      return LHS.Rank < RHS.Rank;
    });

    // One budget for the whole set rather than one per local. A frame holding a
    // reference to a compiler's pass manager reaches everything the compiler
    // owns in two hops, and a per-local budget lets each of forty locals spend
    // it: measured at 8.6 kB of interior, against 5 kB for the backtrace it was
    // meant to annotate.
    unsigned Budget = MaxTerminalLocalNodes;
    size_t Rendered = 0;
    std::vector<StringRef> Starved;
    for (const Local &L : Ordered) {
      // Stopped rather than called with an empty budget. A call with none returns
      // an elision marker, and twenty-five of those was 901 characters spending
      // 36 apiece to say one thing twenty-five times. What a reader needs from a
      // local that got nothing is the name, so that the next plan can capture it.
      if (Budget == 0) {
        Starved.push_back(L.Name);
        continue;
      }

      ValueObjectNode Node(L.Value);
      SerializeValueOptions SOpts;
      // A slice of the shared budget rather than the whole of it, so that the
      // first local to reach a large object cannot spend the response on itself,
      // and only what the slice was spent down to is charged back.
      unsigned Slice = std::min(Budget, MaxNodesPerTerminalLocal);
      const unsigned Given = Slice;
      SOpts.SharedBudget = &Slice;
      SOpts.MaxRenderedChars = MaxTerminalLocalChars;
      SOpts.SawSummary = &m_saw_summary;
      SOpts.SawExpansion = &m_saw_expansion;
      Terminal.Locals[L.Name] = SerializeValue(Node, SOpts);
      Budget -= Given - Slice;
      ++Rendered;
    }

    // One marker for all of them, naming as many as a caller can act on. The
    // budgets are documented as replacing what they cut rather than dropping it
    // silently, and a merged marker honours that at a tenth of the cost. The
    // names are in the order the budget would have reached them, so the ones
    // nearest to having been shown are the ones named.
    if (std::string Note = ElidedLocals(
            Starved, Locals->GetSize() - Injected - Rendered - Starved.size());
        !Note.empty())
      Terminal.Locals["_elided"] = std::move(Note);
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

  // Taking the target off the list is not enough to release it, and dropping
  // the last reference below will not either: a target that has run holds
  // strong references back to itself -- the search filter it shares between
  // breakpoints is one, and each breakpoint keeps a copy of one -- so the last
  // reference is never the last. Destroy is what breaks them, which is why
  // every other way of removing a target pairs the two, and why Debugger::Clear
  // does the same for the targets still on the list at shutdown. A target
  // removed early is out of that loop's reach, so it has to be destroyed here
  // or it, its process and every module it loaded stay allocated for as long as
  // the debugger does.
  m_target->Destroy();
  m_target.reset();

  // The stderr file has been read into the response by now, so nothing outlives
  // the run that needs it. Unlike the artifact, which is reported by path and is
  // the caller's to read afterwards.
  if (m_stderr_reader != sys::fs::kInvalidFile) {
    sys::fs::closeFile(m_stderr_reader);
    m_stderr_reader = sys::fs::kInvalidFile;
  }
  if (!m_stderr_path.empty()) {
    sys::fs::remove(m_stderr_path);
    m_stderr_path.clear();
  }
}

namespace {

/// The observations that were hit, quoted, for a sentence that has to say which
/// tracepoints the numbers beside it came from.
///
/// Only the ones that were hit, because a plan's other observations contributed
/// nothing to the count and naming them would make the sentence disagree with
/// `plan_report`. Bounded, and the remainder counted: a plan of twenty
/// observations would otherwise put twenty labels in one sentence, which is the
/// list `plan_report` already is.
std::string HitLabels(ArrayRef<std::unique_ptr<ObservationSite>> Sites) {
  constexpr size_t MaxNamed = 3;
  std::string Out;
  size_t Named = 0, Beyond = 0;
  for (const std::unique_ptr<ObservationSite> &Site : Sites) {
    if (Site->TotalHits() == 0)
      continue;
    if (Named == MaxNamed) {
      ++Beyond;
      continue;
    }
    if (Named != 0)
      Out += ", ";
    Out += formatv("\"{0}\"", Site->Obs->Label).str();
    ++Named;
  }
  if (Beyond != 0)
    Out += formatv(" and {0} more", Beyond).str();
  return Out;
}

} // namespace

Expected<ObservationResult> ObservationEngine::Run() {
  m_start = Clock::now();
  m_running_since = m_start;
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

  m_running_since = Clock::now();
  m_last_progress = m_running_since;
  m_result.SetupMs =
      ToMs(std::chrono::duration_cast<Micros>(m_running_since - m_start));

  const Outcome Result = WaitForEnd();

  // Before anything else, because the counters the injected code keeps are only
  // readable while the process holding them is there, and for a program that
  // exited they are whatever the stop on its way out read.
  TakeWhatTheProgramRecorded(/*RunHasEnded=*/true);

  FlushHeldEvents();
  DrainInferiorOutput();
  CollectTerminalEvent(Result);

  m_result.Result = Result;
  m_result.ElapsedMs =
      ToMs(std::chrono::duration_cast<Micros>(Clock::now() - m_start));
  m_result.Aggregate = m_aggregator.Render();
  m_result.Profile = m_profile.Render();

  // A run whose tracepoints all resolved and none of which ever fired. Said in
  // one sentence because it is the one conclusion the per-observation numbers do
  // not draw: each one reports `"hits": 0` truthfully, and a caller reading a
  // list of them has to notice that every entry says the same thing before it can
  // ask why. Measured on a run that spent 325 seconds arriving at that list.
  //
  // Only when something resolved. Where nothing did, the observation's own error
  // names what matched no code and suggests the nearest thing that would have,
  // which is a better answer than this one and already there.
  //
  // `no_progress_seconds` is named and no value is suggested, and the default
  // stays off. A plan whose tracepoints legitimately fire near the end of a long
  // run is not a plan to cut short, and nothing here knows which kind this was:
  // two agents that reached this state picked 40 and 20 seconds, neither from
  // anything the run had told them.
  // Read from the sites rather than from the reports, which are filled in
  // further down: reading them here reported that nothing had been hit on a run
  // whose own `plan_report` said a hundred hits, which is worse than saying
  // nothing.
  //
  // Locations are read from the breakpoint for the same reason the report re-reads
  // them there -- a name in a library that loaded during the run resolves when it
  // loads, and the count taken before the launch would call that unresolved.
  const uint64_t Hits = std::accumulate(
      m_sites.begin(), m_sites.end(), uint64_t{0},
      [](uint64_t Sum, const std::unique_ptr<ObservationSite> &S) {
        return Sum + S->TotalHits();
      });
  const bool AnyResolved =
      any_of(m_sites, [](const std::unique_ptr<ObservationSite> &S) {
        return S->Breakpoint && LocationsInTheProgram(*S->Breakpoint) != 0;
      });
  if (Hits == 0 && AnyResolved)
    m_result.Notes.push_back(
        formatv("no tracepoint in this plan was hit in the {0:F2} s the run "
                "took, though every location resolved: the code they name was "
                "never reached. \"no_progress_seconds\" ends a run like this "
                "early, and is off unless asked for because a plan whose "
                "tracepoints only fire near the end of a long run is "
                "legitimate.",
                m_result.ElapsedMs / 1000.0)
            .str());

  // The complementary case, and the one that manufactures a wrong answer rather
  // than an empty one. A ceiling that ends a run leaves an aggregate over the
  // hits that happened, shaped exactly like an aggregate over the program: the
  // value the program would have held at a hit the run never reached is absent
  // from `values` and absent from `outliers`, where absent already means "never
  // occurred". This is the argument the `command` tool makes for reporting a
  // command its ceiling ended as one, made at the field callers are told is
  // usually the answer.
  //
  // The rate is here because it is what a caller sizes the next ceiling with, and
  // it is the one number neither the elapsed time nor the hit count gives alone.
  //
  // A ceiling stop and not merely an abnormal one. A crash is the program reaching
  // its own end, so its aggregate holds every hit the program made and there is no
  // ceiling to raise; `outcome: crashed` is the answer there, and it is the first
  // field of the response. Only where there were hits, too: with none the aggregate
  // is empty and never rendered, so there is nothing to mistake for a summary.
  if (IsCeilingStop(m_result.Result) && Hits != 0) {
    m_result.AggregateCoversPrefix = true;

    // Of the running time rather than the elapsed, because that is what the
    // ceiling counted and what raising it would extend. Floored so that a run
    // that ended in its first millisecond divides by something.
    const double RunningSeconds =
        std::max(m_result.ElapsedMs - m_result.SetupMs, 1.0) / 1000.0;
    m_result.Notes.push_back(
        formatv("the ceiling ended this run with the program still working, so "
                "\"aggregate\" summarises the {0} hits of {1} reached in {2:F1}s "
                "-- about {3} a second -- and no later ones. A value missing "
                "from \"values\" or \"outliers\" may be one the run never "
                "reached rather than one the program never held. "
                "\"aggregate_covers\" says this as a field; raise "
                "\"timeout_seconds\" past what that rate implies to cover the "
                "whole program.",
                Hits, HitLabels(m_sites), RunningSeconds,
                static_cast<uint64_t>(Hits / RunningSeconds))
            .str());
  }

  if (m_result.Output.Truncated)
    m_result.Notes.push_back(
        formatv("only the last {0} bytes of each of the program's own output "
                "streams are reported; the earlier output was dropped.",
                MaxInferiorOutput)
            .str());

  // Said only when both halves hold: something was expanded into its members,
  // and nothing anywhere in the run was rendered by a formatter. Either alone
  // says nothing. A run that expanded a struct of integers expanded exactly what
  // it should have, and a run where any value got a summary has formatters that
  // work and merely does not have one for this type.
  //
  // Worth a note at all because the two cases are indistinguishable in the
  // response and want opposite responses from the caller. A capture of
  // `llvm::StringRef` with no formatter loaded comes back as two fields with the
  // text one level down inside a `const char *`, which reads as the value the
  // program holds -- so a caller re-spells the capture to reach the field, and
  // keeps doing that for every type the project has a formatter for.
  if (m_saw_expansion && !m_saw_summary)
    m_result.Notes.push_back(
        "no data formatter matched any value read in this run, so a value with a "
        "custom rendering came back expanded into its members rather than as the "
        "one thing it stands for. Formatters are not built in: a project ships a "
        "script to import, and this session loads what \"~/.lldbinit\" imports.");
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
    m_result.Cycle = DetectCycle(m_tail_entries);

  uint64_t Emitted = 0;
  for (const std::unique_ptr<ObservationSite> &Site : m_sites) {
    Emitted += Site->Emitted;
    ObservationReport &Report = m_result.Observations[Site->Index];
    Report.Hits = Site->TotalHits();
    Report.ConditionTrue = Site->TotalConditionTrue();
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
      Report.ResolvedLocations = LocationsInTheProgram(*Site->Breakpoint);
      if (Report.ResolvedLocations != 0)
        Report.ResolutionError.reset();
    }

    for (const ObservationSite::CaptureState &Capture : Site->Captures) {
      CaptureReport Rendered;
      Rendered.Expr = Capture.Expr;
      Rendered.FixedExpr = Capture.FixedExpr;
      Rendered.Tier = Capture.Tier;
      Rendered.Evaluations = Capture.Evaluations;
      Rendered.Errors = Capture.Errors;
      Rendered.TotalMs = ToMs(Capture.Spent);
      Rendered.Disabled = Capture.Disabled;
      // The total the decision was taken against was the hits reached by then,
      // which for a capture stopped early is a small fraction of the run and
      // rendered as `"total_hits": 66` beside `"observed_hits": 66`. Corrected
      // here, where the run is over and the real total is known, so that the pair
      // is the share of the program the capture actually covered.
      if (Rendered.Disabled)
        Rendered.Disabled->TotalHits = Site->TotalHits();
      Rendered.FromABI =
          Site->Obs->OnReturn && Capture.Expr == ReturnValueCapture;
      Rendered.InProcess = Capture.InProcess;
      Report.Captures.push_back(std::move(Rendered));

      // Read after the run rather than at the failure, which is the same rule
      // an unresolved tracepoint location follows just above: a capture that
      // failed early and resolved later is reported as resolved, because what
      // the caller needs to know is whether the value was ever readable and not
      // whether the first attempt worked. So a capture still failing at every
      // one of its evaluations is what earns an entry, and one that recovered
      // silently drops out.
      if (Capture.Evaluations == 0 || Capture.Errors != Capture.Evaluations)
        continue;
      if (Site->Obs->OnReturn && Capture.Expr == ReturnValueCapture)
        continue;

      CaptureFailureReport Failure;
      Failure.Label = Site->Obs->Label;
      Failure.Expr = Capture.Expr;
      Failure.Kind = Capture.Failure;
      Failure.Reason = Capture.FailureReason;
      Failure.Candidates = Capture.Candidates;
      Failure.Disabled = Capture.Disabled.has_value();
      // An adopted fixit and a merely suggested one are both worth reporting
      // and must not read alike: one is what ran, the other is what the
      // compiler guessed and could not make work either.
      if (!Capture.FixedExpr.empty()) {
        Failure.FixedExpr = Capture.FixedExpr;
        Failure.FixApplied = true;
      } else if (!Capture.SuggestedFix.empty()) {
        Failure.FixedExpr = Capture.SuggestedFix;
      }
      m_result.CaptureFailures.push_back(std::move(Failure));
    }

    // A condition that never once evaluated is not a condition that never held.
    // The first reads as an observation whose code ran and whose values were
    // all uninteresting, which is the one conclusion it must not be allowed to
    // support, since a hit whose condition failed is neither captured nor
    // emitted.
    //
    // Counted against the times the condition was evaluated rather than against
    // the observation's hits: the two differ by `skip_first`, whose hits never
    // reach the condition at all, and comparing with the hits would silently
    // drop the report for any plan that skipped one.
    const uint64_t ConditionEvaluated =
        Site->ConditionTrue + Site->ConditionErrors;
    if (Site->Obs->WhenExpr && ConditionEvaluated != 0 &&
        Site->ConditionErrors == ConditionEvaluated) {
      CaptureFailureReport Failure;
      Failure.Label = Site->Obs->Label;
      Failure.Expr = *Site->Obs->WhenExpr;
      Failure.Kind = Site->WhenFailure;
      Failure.Reason = Site->WhenFailureReason;
      Failure.Candidates = Site->WhenCandidates;
      Failure.IsCondition = true;
      // Never: a condition is not turned off, because a run that stopped
      // evaluating it would be recording different hits rather than fewer.
      Failure.Disabled = false;
      if (!Site->WhenFixedExpr.empty()) {
        Failure.FixedExpr = Site->WhenFixedExpr;
        Failure.FixApplied = true;
      } else if (!Site->WhenSuggestedFix.empty()) {
        Failure.FixedExpr = Site->WhenSuggestedFix;
      }
      m_result.CaptureFailures.push_back(std::move(Failure));
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
              Site->Obs->Label, Report.ReturnsAbandoned, Site->TotalHits())
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
    // Inlined only when the program ended badly, and only when there is a
    // sequence to read. A run that ended badly is read backwards from the end;
    // one that ended well is read through its aggregate, and the events are a
    // file away either way. A single event is not a sequence: the aggregate of
    // one hit already holds every value it holds, so inlining it repeats the
    // densest part of the response -- measured at 647 characters of a
    // single-hit run, saying what its 295-character aggregate had said.
    if (IsAbnormal(Result) && m_tail_events.size() > 1)
      Artifact.Tail = m_tail_events;
    Artifact.CarriesBacktrace = any_of(m_sites, [](const auto &Site) {
      return Site->Obs->Backtrace > 0;
    });
    m_result.Artifact = std::move(Artifact);
  }

  return std::move(m_result);
}
