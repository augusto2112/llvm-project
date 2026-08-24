//===- ObservationEngine.h ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_PROTOCOL_MCP_OBSERVATIONENGINE_H
#define LLDB_SOURCE_PLUGINS_PROTOCOL_MCP_OBSERVATIONENGINE_H

#include "Aggregate.h"
#include "EventArtifact.h"
#include "ObservationPlan.h"
#include "lldb/Target/DWIMValueResolution.h"
#include "lldb/lldb-forward.h"
#include "lldb/lldb-private-interfaces.h"
#include "lldb/lldb-types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lldb_private::mcp {

/// How a run ended. Every run ends one of these four ways, and each carries a
/// different next step for the caller, so a run that produced no observations
/// is still not an empty result.
enum class Outcome {
  Exited,
  Crashed,

  /// The wall-clock ceiling was reached with the program still running.
  TimedOut,

  /// No tracepoint in the plan was hit for longer than the plan allowed.
  /// Distinct from \ref TimedOut because the program was making progress of
  /// some kind; what stopped was the part of it the plan was watching.
  NoProgress,
};

llvm::StringRef ToString(Outcome O);

/// Whether an outcome means the program did not reach its own end. The last
/// few events are inlined for these and not for a clean exit: a program that
/// ended badly is read backwards from the end, and a program that ended well
/// is read through its aggregate.
bool IsAbnormal(Outcome O);

/// Events inlined in the response when the program ended abnormally. Enough to
/// show what the program was doing as it died, few enough to leave the response
/// dominated by the aggregate.
constexpr size_t InlinedTailEvents = 5;

//===----------------------------------------------------------------------===//
// Emission
//===----------------------------------------------------------------------===//

/// What becomes of one hit's event.
enum class EmitDecision {
  Emit,
  Skip,

  /// Keep as the observation's pending final event, displacing whatever was
  /// already pending, and write it when the run ends. Which hit is the last one
  /// is not knowable at the hit itself, so the last one is whichever is still
  /// being held when there are no more.
  Hold,
};

llvm::StringRef ToString(EmitDecision D);

struct EmitDecisionInput {
  EmitMode Mode = EmitMode::EveryHit;

  /// The rendered capture tuple of the previous hit this observation recorded,
  /// unset at the first one. Comparison is against the previous hit rather than
  /// the previous emitted event, so that reducing the stream does not also
  /// change what counts as a change.
  std::optional<std::string> Previous;

  /// The rendered capture tuple of the hit being decided. An observation with
  /// no captures renders the same empty tuple at every hit, which is why
  /// `on_change` on a bare tracepoint emits once and not never.
  std::string Current;

  /// Which of the observation's recorded hits this is, counting from 1. A hit
  /// the ignore count swallowed or whose condition was false is not one.
  uint64_t HitIndex = 1;

  /// Hits skipped before any was recorded. Only \ref OnlyHit reads this, and
  /// only to offset its numbering.
  uint32_t SkipFirst = 0;

  /// Records a single hit, numbered the way the aggregate numbers the hit it
  /// names: \ref SkipFirst plus \ref HitIndex. Both sides have to compute it
  /// the same way, because reading an interesting hit out of the aggregate and
  /// asking for that hit here is the loop the tool is built around, and a
  /// numbering that disagreed by the skipped hits would select a different one.
  std::optional<uint32_t> OnlyHit;
};

/// Decides what becomes of one hit's event. Emission is the only reduction in
/// the design that can lose information, so it is separated from the machinery
/// that collects hits and is decided here alone.
EmitDecision DecideEmit(const EmitDecisionInput &In);

//===----------------------------------------------------------------------===//
// Expression cost control
//===----------------------------------------------------------------------===//

/// The share of the time left before the timeout that one capture's projected
/// cost may take up before the capture is turned off.
constexpr unsigned CaptureCostBudgetDivisor = 4;

struct CaptureCostInput {
  /// Which mechanism the capture resolved through. Only a tier that compiles
  /// and runs code in the inferior is ever turned off: a variable path is a
  /// debug-info lookup and a memory read, and no budget makes giving that up
  /// worth the data it costs.
  ValueResolutionTier Tier = ValueResolutionTier::Expression;

  /// Wall time spent evaluating this capture so far.
  std::chrono::microseconds Spent{0};

  /// Hits that time was spent over.
  uint64_t ObservedHits = 0;

  /// Hits the observation has taken, including any this capture was not
  /// evaluated at. Reported so that a partial capture says how partial it is.
  uint64_t TotalHits = 0;

  /// Time left before the run's wall-clock ceiling.
  std::chrono::microseconds Remaining{0};

  /// Evaluations that produced no value. A capture that has failed at every one
  /// of them names something that does not resolve where the observation is
  /// taken, which is a different fault from costing too much and wants a
  /// different remedy.
  uint64_t Errors = 0;

  /// Whether the observation is taken as the function returns, where its frame
  /// is already gone. That is the likeliest reason a capture resolves nowhere,
  /// and it has a different fix from a name that is merely out of scope, so the
  /// note has to be able to tell them apart.
  bool AtReturn = false;

  /// The capture expression, so that the note can name a cheaper form of it.
  std::string Expr;
};

/// Attempts a capture gets before failing at all of them is read as a name that
/// cannot resolve at this location rather than as bad luck. One is too few: a
/// pointer that is null at the first hit and set at the second is worth keeping.
/// The cost of being wrong here is bounded by the note, which says what was
/// stopped and why, so a caller that meant it can ask again.
constexpr uint64_t UnresolvableCaptureAttempts = 3;

/// Why a capture was, or was not, turned off, in the terms the report needs.
struct CaptureCostDecision {
  bool Disable = false;
  uint64_t ObservedHits = 0;
  uint64_t TotalHits = 0;
  double PerHitMs = 0.0;

  /// What the capture is projected to cost over the rest of the run.
  double ProjectedMs = 0.0;

  /// Time the projection was measured against.
  double RemainingMs = 0.0;

  /// What the caller should write instead. Set only when the capture was turned
  /// off: partial data plus the fix is worth more than a flag the caller had no
  /// basis on which to set.
  std::string Note;

  llvm::json::Value Render() const;
};

/// Decides whether a capture has grown too expensive to keep evaluating.
///
/// The cost already paid is the estimate of the cost still to come: a trigger
/// that fired often enough to matter so far will keep firing. So a capture is
/// turned off once what it has already spent would, spent again, take more than
/// its share of the time left.
CaptureCostDecision AssessCaptureCost(const CaptureCostInput &In);

/// The path form of a getter call, when \p Expr is one. The convention that
/// pairs `getX()` with a readable member is what makes the substitution sound;
/// it turns a compile and a call in the inferior into a memory read.
std::optional<std::string> SuggestPathForm(llvm::StringRef Expr);

//===----------------------------------------------------------------------===//
// Frame arming
//===----------------------------------------------------------------------===//

/// The frames whose departure is being waited for, at breakpoints on the
/// addresses they return to.
///
/// A frame is identified by the thread it runs on together with its call-frame
/// address, because a return address is not an identity. Two threads reach the
/// same one, so waiting on the address alone reports one thread's return as
/// another's. A frame unwound by an exception or a longjmp never reaches it at
/// all, so waiting on the address alone also waits forever -- and then reports
/// the next arrival there as the return of a frame that is long gone.
///
/// The stack grows down, so a frame is known to have been left as soon as its
/// thread is seen in a frame at or below it. That is the whole test, and it
/// costs no unwinding: the current frame's own call-frame address is enough.
class ArmedFrames {
public:
  /// Records that \p Tid is in a frame at \p CFA that will return to \p Return.
  void Arm(lldb::tid_t Tid, lldb::addr_t Return, lldb::addr_t CFA);

  /// Whether one of \p Tid's armed frames returned to \p Return, given that the
  /// thread is now in the frame at \p CFA. Frames the thread has left without
  /// returning are dropped here rather than reported, and counted in
  /// \ref GetAbandoned.
  bool Returned(lldb::tid_t Tid, lldb::addr_t Return, lldb::addr_t CFA);

  /// Whether an armed frame of \p Tid still encloses the frame at \p CFA.
  ///
  /// This is what makes a `called_from` gate exact: being called from a
  /// function is having one of its frames still below this one on this thread,
  /// which is not the same as the gate having been entered at some point by
  /// somebody.
  bool EnclosesFrame(lldb::tid_t Tid, lldb::addr_t CFA);

  /// Whether no frame is armed on any thread, which is when the breakpoints
  /// behind them can be switched off.
  bool Empty() const { return m_count == 0; }

  /// Frames proven to have been left without returning, which is what an
  /// exception or a longjmp does to one. A frame is proven gone by a later
  /// frame of the same thread, so the last call of a run is not among these;
  /// \ref GetOutstanding is what accounts for that one.
  uint64_t GetAbandoned() const { return m_abandoned; }

  /// Frames still armed. Once the program has exited these are frames that
  /// never returned either, but while it is running they may simply not have
  /// returned yet, and the difference is not this class's to decide.
  uint64_t GetOutstanding() const { return m_count; }

private:
  struct Frame {
    lldb::addr_t Return;
    lldb::addr_t CFA;
  };

  /// Drops the frames of \p Frames that a thread found at \p CFA has left, and
  /// counts them as abandoned. Armed frames run outermost-first, so the
  /// departed ones are always a suffix.
  void Discard(llvm::SmallVectorImpl<Frame> &Frames, lldb::addr_t CFA);

  /// Innermost last, per thread. Erased once a thread has no armed frame, so
  /// that a program churning through threads does not grow this indefinitely.
  llvm::DenseMap<lldb::tid_t, llvm::SmallVector<Frame, 4>> m_frames;

  uint64_t m_count = 0;
  uint64_t m_abandoned = 0;
};

//===----------------------------------------------------------------------===//
// Frame ranking
//===----------------------------------------------------------------------===//

/// One frame as the unwinder described it.
struct RawFrame {
  std::string Function;

  /// Empty when the frame resolved to no source file.
  std::string File;

  uint32_t Line = 0;
};

/// One frame as the terminal event reports it.
struct RankedFrame {
  std::string Function;
  std::string File;
  uint32_t Line = 0;

  /// Consecutive frames of the same function folded into this entry, 1 when
  /// nothing was folded.
  uint32_t Repeats = 1;

  /// Whether the frame's file lies outside the source tree.
  bool IsSystem = false;

  llvm::json::Value Render() const;
};

/// Whether \p File belongs to a toolchain or system location rather than to the
/// program being observed. Judged from the path because that is the only
/// evidence present in every backtrace: a name says nothing about whether its
/// definition is one the caller can change.
bool IsSystemSourcePath(llvm::StringRef File);

/// Orders a backtrace so that the frames worth reading come first.
///
/// Raw unwinder order is close to useless once a stack is deep: an assertion
/// inside a compiler pass arrives under a hundred frames of pass-manager
/// plumbing, and a runaway recursion arrives as one frame repeated until the
/// unwinder gives up. Three reductions apply in order — consecutive frames of
/// one function fold into a single entry carrying the count, frames that
/// resolved no source are dropped, and what remains is stably partitioned so
/// that source-tree frames precede system ones.
///
/// A binary with no debug information at all would lose every frame to the
/// second reduction, so when no frame has source the unresolved frames are
/// kept: where the program is remains the answer even when the source does not
/// exist.
std::vector<RankedFrame> RankFrames(llvm::ArrayRef<RawFrame> Frames);

//===----------------------------------------------------------------------===//
// Result
//===----------------------------------------------------------------------===//

/// What one capture resolved to and what it cost.
struct CaptureReport {
  std::string Expr;
  ValueResolutionTier Tier = ValueResolutionTier::Unresolved;

  /// Hits the capture was evaluated at.
  uint64_t Evaluations = 0;

  /// Evaluations that produced no value.
  uint64_t Errors = 0;

  double TotalMs = 0.0;

  /// Set when cost control turned the capture off partway through the run.
  std::optional<CaptureCostDecision> Disabled;

  /// Set for `$return`, which is read from the ABI's result location rather
  /// than resolved from a name, so it has neither a tier nor a per-hit cost.
  bool FromABI = false;

  llvm::json::Value Render() const;
};

/// What one observation did over the run.
struct ObservationReport {
  std::string Label;
  std::string At;

  /// Zero means the observation could not fire. Reported even though it is a
  /// number the caller did not ask for, because it is the one thing that
  /// otherwise looks identical to code that never ran.
  uint32_t ResolvedLocations = 0;

  /// Why nothing matched, with the nearest names that did.
  std::optional<std::string> ResolutionError;

  /// Times the location was reached, counting the hits that were skipped and
  /// those whose condition was false. This is the number that answers whether
  /// the code ran at all, so nothing is filtered out of it.
  uint64_t Hits = 0;

  /// Hits whose condition held, and hits whose condition could not be
  /// evaluated. Kept apart from \ref Hits and from each other because the three
  /// answer different questions — whether the code ran, whether the condition
  /// ever held, and whether the condition was even valid — and a caller that
  /// cannot tell them apart has to guess which of three fixes to apply.
  /// Meaningful only when the observation carried a condition.
  uint64_t ConditionTrue = 0;
  uint64_t ConditionErrors = 0;
  bool HasCondition = false;
  double ConditionMs = 0.0;

  /// Hits whose event was written to the event stream. Well below \ref Hits is
  /// the point of a reducing emission mode rather than a sign of loss, since
  /// the aggregate is computed over every hit.
  uint64_t Emitted = 0;

  /// Threads that hit this observation. Reported once it is more than one,
  /// because hit order, change detection and the aggregate are all kept per
  /// observation and not per thread: past one thread, the sequence a reader
  /// sees is an interleaving, and events carry the thread each hit came from.
  uint32_t Threads = 0;

  /// Frames of an `on: return` observation that were left without returning,
  /// which is what an exception or a longjmp does to one. Reported because it
  /// is otherwise indistinguishable from a return that went unnoticed.
  uint64_t ReturnsAbandoned = 0;

  std::vector<CaptureReport> Captures;

  llvm::json::Value Render() const;
};

/// Where the program was when the run ended.
///
/// A timeout and a stall get exactly what a crash gets. A hang is a terminal
/// event, not the absence of one, and the state at the moment a program stopped
/// making progress is the whole of what the caller needs.
struct TerminalEvent {
  /// The stop as the target described it, or how the program exited.
  std::string Description;

  std::optional<int> ExitStatus;

  /// The innermost frame with source, which is where a reader starts.
  std::string Function;
  std::string File;
  uint32_t Line = 0;

  std::vector<RankedFrame> Frames;

  /// Frames the unwinder produced, against which the ranked list is a
  /// reduction.
  uint32_t FramesTotal = 0;

  /// Frames dropped from \ref Frames after ranking.
  uint32_t FramesOmitted = 0;

  llvm::json::Object Locals;

  /// Source around \ref Line, when the file could be read.
  std::string Source;

  llvm::json::Value Render() const;
};

/// Where the full event stream went.
struct ArtifactReport {
  /// Empty when no file could be opened, in which case the events exist only as
  /// \ref Tail and as the aggregate.
  std::string Path;

  uint64_t Events = 0;

  /// Whether the artifact stops short of the run.
  bool Truncated = false;

  /// The last few events, inlined only when the program ended abnormally.
  std::vector<llvm::json::Value> Tail;

  /// Whether any observation asked for a backtrace, which is what puts a
  /// `frames` on an event. The field list is checked against what a line
  /// actually carries, so advertising it unconditionally would send a reader
  /// looking for a field that is not there on a plan that never asked for one.
  bool CarriesBacktrace = false;

  llvm::json::Value Render() const;
};

struct ObservationResult {
  Outcome Result = Outcome::Exited;

  std::vector<ObservationReport> Observations;

  /// What every capture was observed to hold, over every hit rather than over
  /// the emitted events.
  llvm::json::Value Aggregate = nullptr;

  /// A repeating block the end of the run kept traversing. For a program that
  /// did not terminate this is the answer.
  std::optional<CycleReport> Cycle;

  TerminalEvent Terminal;

  std::optional<ArtifactReport> Artifact;

  /// The program's own output, which is often the only evidence of how far it
  /// got.
  std::string InferiorOutput;

  double ElapsedMs = 0.0;

  /// Of that, what was spent before the program started running: creating the
  /// target, reading its debug info, and resolving the tracepoints.
  ///
  /// Reported separately because the two scale with different things and a
  /// caller decides whether to run again on which one dominates. Measured on a
  /// 238 MB debug build of a compiler, the first run in a session took 9.1 s and
  /// the second, identical, 0.23 s: the cost is indexing that binary, it is
  /// charged once per image rather than per hit, and a caller told only the total
  /// concluded that tracepoints cost 250x and stopped using them for
  /// measurement.
  double SetupMs = 0.0;

  /// What went wrong that no single observation owns.
  std::vector<std::string> Notes;

  llvm::json::Value Render() const;
};

//===----------------------------------------------------------------------===//
// Engine
//===----------------------------------------------------------------------===//

/// Per-observation collection state. Defined by the implementation; a pointer
/// to one is the baton the breakpoint callbacks are installed with.
struct ObservationSite;

/// Breakpoints at the addresses frames return to, together with the frames
/// armed at them. Defined by the implementation.
struct ReturnSet;

/// Runs an observation plan and reports what happened.
///
/// Collection is driven by breakpoint callbacks that decline the stop, so the
/// program runs to its own end rather than being stepped: the cost of an
/// observation is a callback per hit, not a round trip per line. Callbacks are
/// installed synchronous because an asynchronous one runs after the stop has
/// already been broadcast, which is too late to keep the stop from surfacing.
///
/// A capture that reaches the expression evaluator therefore runs code in the
/// observed process from inside stop processing, which costs a temporary state
/// thread per evaluation on top of the compile and the call. That cost is the
/// reason a capture is measured and can be turned off mid-run, and the reason a
/// path-shaped capture is worth preferring wherever one exists.
class ObservationEngine {
public:
  ObservationEngine(Debugger &Dbg, ObservationPlan Plan);
  ~ObservationEngine();

  ObservationEngine(const ObservationEngine &) = delete;
  ObservationEngine &operator=(const ObservationEngine &) = delete;

  /// Runs the plan to completion and tears the process down.
  ///
  /// An error comes back only when the run could not be started — no such
  /// program, no target. Everything that can go wrong once the program is
  /// running is part of the result, because a run that crashed, hung or
  /// observed nothing has produced the answer rather than failed to.
  ///
  /// One engine runs one plan once; the result is moved out of the engine.
  llvm::Expected<ObservationResult> Run();

private:
  friend struct ObservationSite;

  /// Records one hit. Returns whether the process should stop, which is true
  /// only when the run has to end: the mechanism that makes a tracepoint cheap
  /// is that this normally declines the stop.
  bool RecordHit(ObservationSite &Site, StoppointCallbackContext *Ctx);

  /// Arms a breakpoint at the return address of \p Ctx's frame, so that leaving
  /// a frame can be observed without unwinding at every hit of something else.
  /// Unwinding per hit to ask who called is what makes the obvious
  /// implementation cost stack depth times hits.
  void ArmReturn(ReturnSet &Set, StoppointCallbackContext *Ctx,
                 BreakpointHitCallback Callback, void *Baton);

  llvm::Error InstallObservations();
  llvm::Error Launch();
  Outcome WaitForEnd();
  void CollectTerminalEvent(Outcome Result);
  void DrainInferiorOutput();
  void FlushHeldEvents();
  void WriteEvent(ObservationSite &Site, llvm::json::Object Event);

  /// Kills the observed process and drops the target. Runs however the run
  /// ended, including on the paths that never got the program started: a target
  /// left behind holds this run's breakpoints, and in a session somebody else
  /// is using it also outlives the call that made it.
  void Teardown();

  /// Records that the plan saw the program move. The stall ceiling is measured
  /// against this rather than against the event stream, because a mode that
  /// reduces the stream is not the program slowing down.
  void NoteProgress();

  ValueResolutionOptions CaptureOptions() const;

  std::chrono::microseconds Remaining() const;

  /// Whether the run is out of time, and on which of the two clocks.
  bool ShouldEnd(Outcome &Reason);

  /// Records that the run has to end, and reports whether it does. A callback
  /// returns this so that a program busy hitting tracepoints, which never gives
  /// the wait loop a chance to look at the clock, still stops on time.
  bool EndIfDue();

  Debugger &m_debugger;
  ObservationPlan m_plan;
  lldb::TargetSP m_target;

  std::vector<std::unique_ptr<ObservationSite>> m_sites;

  Aggregator m_aggregator;
  std::unique_ptr<EventArtifact> m_artifact;

  /// Locations of the hits most recently recorded, which is what a cycle is
  /// found over. The hit sequence rather than the emitted one, since an
  /// emission mode that drops repeats would destroy the very repetition being
  /// looked for.
  std::vector<std::string> m_tail_labels;

  std::vector<llvm::json::Value> m_tail_events;

  /// Events written so far, which numbers them across all observations so that
  /// one label's event can be placed against another's.
  uint64_t m_seq = 0;

  std::chrono::steady_clock::time_point m_start;

  /// When the plan last saw the program move, which is the last hit any
  /// observation took rather than the last event written.
  std::chrono::steady_clock::time_point m_last_progress;

  /// Set by a callback that found the run has to end, and read by the wait
  /// loop. A callback is the only place a run that keeps hitting tracepoints
  /// can notice that its time is up.
  std::optional<Outcome> m_requested_end;

  bool m_output_truncated = false;

  ObservationResult m_result;
};

} // namespace lldb_private::mcp

#endif
