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
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
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

/// Whether the run was ended by one of the plan's ceilings rather than by the
/// program.
///
/// Narrower than \ref IsAbnormal, and the distinction is load-bearing wherever a
/// response tells the caller that running for longer would have shown it more. A
/// crash is abnormal and is still the program reaching its own end: nothing it
/// would have done later is missing from what was collected, and no ceiling would
/// have changed that. Told otherwise, the crashed side of a comparison was
/// informed that its ceiling had ended it after a fifth of a second against a
/// ninety-second one.
bool IsCeilingStop(Outcome O);

/// Both terms of a run's wall clock, as a parenthetical for the sentence that
/// reports a stop the ceiling delivered.
///
/// A ceiling bounds the running time, `elapsed_ms` is the whole call, and the
/// term that closes the gap between them -- `setup_ms` -- is reported only when
/// setup was most of the run, which is exactly what a long hang guarantees it
/// was not. Left with the arithmetic and nothing to do it with, 16 of 38 agent
/// runs concluded that `timeout_seconds` does not mean what it says: 25 seconds
/// asked against 32.0 reported, 40 against 46.4, 45 against 64.6, 90 against
/// 101.1. One stopped at "so I still don't know what it actually bounds", which
/// is a caller who has lost confidence in a number it has to choose before every
/// run.
std::string DescribeWallClock(double RunningMs, double SetupMs);

/// Where one local sits in the order a stopped frame's locals are read, lowest
/// read first.
///
/// A budget shared across the locals and spent in declaration order gives it to
/// whatever the compiler emitted first, which for a C++ member function is
/// `this` and then the formal parameters. Measured on a compiler stopped inside
/// one of its passes: those four took the whole of a 96-node budget and every one
/// of the eleven body locals came back as an elision marker. `this` alone was
/// 4,440 bytes, 37.7% of the response, and -- the part worth being precise about
/// -- it was real pass and target state rather than unreadable field soup. It is
/// simply never what was asked about. What was asked about is the loop-carried
/// value a body local holds.
///
/// Scalars first: one node and about twenty-five bytes each, and they are what a
/// hang is explained by. Then the body's own aggregates, then the parameters,
/// then `this`, which reaches everything its object owns in two hops. Last the
/// compiler's own range-for temporaries, which took eight of thirty-two slots in
/// the frame measured and name nothing a reader would ask about again.
///
/// This decides what is expanded, not what order the response prints in:
/// `llvm::json::Object` prints its keys sorted, and a caller names a local by
/// key.
unsigned TerminalLocalRank(llvm::StringRef Name, bool IsScalar,
                           bool IsArgument);

/// The one marker that stands for every local a stopped frame did not report:
/// \p Starved for the ones the shared node budget ran out before, \p NotRead for
/// the ones past the bound on how many locals are read at all. Empty when there
/// were none of either.
///
/// One marker rather than one per local. A budget is documented as replacing what
/// it cuts rather than dropping it silently, and calling the serializer with
/// nothing left honours that by returning an elision marker for each: measured on
/// one frame, 25 copies of `{"_elided":"node budget"}`, 901 characters announcing
/// absence. A merged marker says the same thing at a tenth of the cost.
///
/// The names are the content. What a caller does with a local it did not get is
/// name it in the next plan's `capture`, and a count alone does not let it.
/// Bounded all the same, because a ninth name is not a ninth thing to do.
std::string ElidedLocals(llvm::ArrayRef<llvm::StringRef> Starved,
                         size_t NotRead);

/// Separates one capture's rendering from the next in the tuple a hit is recorded
/// as. A control character cannot occur inside a rendered value, so two different
/// tuples cannot join into one identical string -- which is what lets an emission
/// mode and a comparison both decide "the same hit" by comparing text.
constexpr char CaptureTupleSeparator = '\x1f';

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

/// Why a capture produced no value, in the only two terms that decide what to
/// do about it: whether asking again can ever answer differently, and where the
/// names that would have worked are to be read from.
enum class CaptureFailure {
  /// Resolvable in principle, but not from where this hit was taken: a value
  /// optimised out, a different inlined location, a null pointer part-way along
  /// a path. The next hit is a fresh question, so the capture is kept.
  Situational,

  /// Nothing of that name is in scope. Candidates are the frame's own
  /// parameters and locals.
  ///
  /// Stable, but only for as long as the loaded modules are: a global defined
  /// in a library that has not been dlopened yet is spelled correctly and does
  /// not resolve, which is why disabling on this must be undone when a module
  /// arrives.
  UnknownName,

  /// The base of the path resolved and the type it resolved to has no such
  /// member. Candidates are that type's fields.
  ///
  /// Stable against module loading as well as against hits: a type's members
  /// come from its own definition, and a library loading later does not add
  /// any.
  UnknownMember,

  /// The expression cannot be parsed, or a member is being read out of
  /// something that has no members at all. No candidate list applies, because
  /// the fault is in the shape of the expression rather than in a name.
  Malformed,
};

llvm::StringRef ToString(CaptureFailure Kind);

/// Whether a failure of \p Kind will recur identically at every remaining hit,
/// which is what makes evaluating it again a cost with no possible return.
bool IsStableFailure(CaptureFailure Kind);

/// Reads \p Diagnostic, the evaluator's account of why a capture failed, as one
/// of the four cases.
///
/// Classified from the diagnostic text because that is the only evidence
/// present: the evaluator reports a parse failure as one `ExpressionResults`
/// value whatever the compiler objected to, and what the compiler objected to
/// is the whole distinction here. Anything unrecognised is \ref Situational, so
/// a diagnostic this does not know is a capture that keeps being tried rather
/// than one wrongly abandoned.
CaptureFailure ClassifyCaptureFailure(llvm::StringRef Diagnostic);

/// The part of \p Diagnostic worth reporting beside \p Kind, bounded to
/// \p MaxChars.
///
/// The line that decided the class, where the message carries more than one
/// objection, so that the reason and the detail beside it are about the same
/// thing. Falls back to the condensed message where no single line owns the
/// class.
std::string DescribeCaptureFailure(llvm::StringRef Diagnostic,
                                   CaptureFailure Kind, unsigned MaxChars);

/// Names offered against a capture that did not resolve.
struct CaptureCandidates {
  std::vector<std::string> Names;

  /// Names there were to choose from, against which \ref Names is a selection.
  /// Reported so that a short list is read as a selection and not as the whole
  /// of what was in scope.
  uint64_t InScope = 0;
};

/// Names offered against one unresolved capture. A wide struct or a frame in a
/// long function has more names than a reader will weigh, and the count of the
/// rest is what keeps a bounded list honest.
constexpr size_t MaxCaptureCandidates = 6;

/// Selects from \p InScope the names worth offering against \p Wanted.
///
/// Those close enough to be a plausible misspelling come first, nearest-first,
/// ranked by the same comparison an unresolved tracepoint location gets. The
/// remainder follow in the order given, because a caller who named something
/// nothing resembles is still answered best by what was actually there --
/// which is the case a pure nearest-name ranking answers with silence.
CaptureCandidates RankCaptureCandidates(llvm::StringRef Wanted,
                                        std::vector<std::string> InScope);

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

  /// How the most recent failure was classified. A capture whose failure is
  /// stable is stopped at the first one rather than after
  /// \ref UnresolvableCaptureAttempts of them: the attempts exist to give a
  /// value that is merely absent here a chance to appear, and a member a type
  /// does not have will not grow one. Meaningful only for the Unresolved tier.
  CaptureFailure Failure = CaptureFailure::Situational;

  /// Locations the observation resolved to.
  ///
  /// A name is looked up at a program counter, and an observation on a function
  /// name can resolve to several -- differently inlined copies of it, each with
  /// its own set of variables in scope. So one failure does not settle a name
  /// that is out of scope: it may be in scope at the next location the
  /// tracepoint fires at, and giving up at the first hit would lose it there.
  /// A member a type does not have, and an expression that does not parse, are
  /// not facts about a location and settle at the first failure regardless.
  uint32_t Locations = 1;

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

  /// Where the unwinder had this frame, so that anything read out of the frame
  /// afterwards is read out of the one the report names. Ranking reorders, folds
  /// and drops frames, so a position in the reported list says nothing about a
  /// position on the stack.
  uint32_t Index = 0;
};

/// One frame as the terminal event reports it.
struct RankedFrame {
  std::string Function;
  std::string File;
  uint32_t Line = 0;

  /// The unwinder's index for the frame this entry's location came from, which is
  /// what `frame select` takes and what the locals beside the report were read
  /// from.
  uint32_t Index = 0;

  /// Consecutive frames of the same function folded into this entry, 1 when
  /// nothing was folded.
  uint32_t Repeats = 1;

  /// Whether the frame's file lies outside the source tree.
  bool IsSystem = false;

  /// \p FileRoot is a directory prefix already reported once beside the list,
  /// which \ref File is emitted relative to. Empty to emit it whole.
  llvm::json::Value Render(llvm::StringRef FileRoot = llvm::StringRef()) const;
};

/// Whether \p File belongs to a toolchain or system location rather than to the
/// program being observed. Judged from the path because that is the only
/// evidence present in every backtrace: a name says nothing about whether its
/// definition is one the caller can change.
bool IsSystemSourcePath(llvm::StringRef File);

/// Replaces the contents of each template argument list in \p Function with an
/// ellipsis, leaving everything a reader identifies the frame by.
///
/// A demangled C++ frame carries its template arguments in full, and in
/// template-heavy code that is most of its length: measured on one frame of a
/// compiler's instruction selector, 323 characters of which 250 were two
/// spellings of an intrusive list iterator, in a backtrace of 23 such frames
/// beside the four that a reader wanted. What the arguments distinguish is one
/// instantiation from another, and the file and line reported next to the name
/// already do that.
///
/// Nesting is tracked rather than matched greedily, so a nested list collapses
/// with the one enclosing it, and `operator<` and `operator<<` keep their names:
/// their angle brackets do not open an argument list.
std::string CollapseTemplateArguments(llvm::StringRef Function);

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
///
/// \p RawDropped, when given, receives the number of raw frames the second
/// reduction discarded, so that a caller can report a total against which the
/// list it prints is a reduction. A shorter list is not a quieter one, and a
/// count that only some of the reductions contribute to reads as if the others
/// never happened.
std::vector<RankedFrame> RankFrames(llvm::ArrayRef<RawFrame> Frames,
                                   uint32_t *RawDropped = nullptr);

/// Where a program spent its time, accumulated from stacks sampled while it ran.
///
/// Every other part of a plan answers a question the caller already knew to ask:
/// a tracepoint has to name a function or a line. That makes the tool a way to
/// confirm a hypothesis and not a way to form one, and for the case it is most
/// wanted for -- a program pinned to a core and producing nothing -- the first
/// thing anybody wants is the opposite: not "is this function running" but "what
/// is running". Sampled stacks answer that without being told where to look.
///
/// The samples are folded as they arrive rather than kept, so a run of any length
/// holds one entry per function seen and one running path per thread.
class StackProfile {
public:
  /// Records one thread's stack, innermost frame first, as the unwinder gives
  /// it.
  void Record(lldb::tid_t Tid, llvm::ArrayRef<RawFrame> InnermostFirst);

  uint64_t Samples() const { return m_samples; }

  /// The profile, or a null value when nothing was sampled -- which is the
  /// ordinary case for a program that finishes before the first sample is due.
  llvm::json::Value Render() const;

  /// The thread whose samples say what the run was doing, or 0 when nothing was
  /// sampled.
  ///
  /// Not the one with the most samples. Every live thread is recorded at every
  /// sample, so a per-thread count measures how long a thread existed and not
  /// what it did, and any two threads alive across the same samples tie exactly:
  /// measured on five subjects, 14 against 14, 14 against 14, 15 against 15. On a
  /// tie an ordered scan hands the answer to the lowest thread id, which is the
  /// one created first. Two programs differing only in the order of two
  /// `pthread_create` calls, both threads spinning identically, reported a
  /// different thread as the busy one.
  ///
  /// Ranked instead on what the samples of each thread contain. First on samples
  /// whose innermost frame resolved to source, which is the same partition a
  /// backtrace is ranked by and for the same reason: a thread parked in a library
  /// wait is sampled as often as one burning a core and resolves nothing. Then on
  /// how many distinct places the thread was found in, because a parked thread has
  /// exactly one innermost frame for the whole run and a working thread's moves.
  /// Then on how long it was alive, and last on the thread id, so that two threads
  /// that really are doing the same thing get a stable answer rather than an
  /// arbitrary one.
  lldb::tid_t BusiestThread() const;

  /// Functions named in `hot`. The innermost frame of every sample is one
  /// function, so this is a bound on how many distinct places the program was
  /// found in, and past a handful the tail is noise however long the run.
  static constexpr size_t MaxHot = 8;

  /// The share of the samples the busiest place has to hold, as a reciprocal,
  /// before naming the places individually says anything. Where self time is spread
  /// thinner than this, one entry stands for the list: measured on a compiler
  /// looping inside one analysis, eight accessors holding one or two samples each
  /// of nineteen, with nine more elided, beside a covering path that named the
  /// function responsible.
  static constexpr uint64_t MinHotShareDivisor = 8;

  /// Samples the busiest place has to hold, whatever share of the run that is,
  /// before naming the places individually says anything.
  ///
  /// A share test alone has no floor. Two samples of sixteen is an eighth exactly,
  /// which passes -- the collapse fires at seventeen samples and not at sixteen --
  /// and a ranking of eight entries holding two, two, one, one, one, one, one, one
  /// is not a ranking. The real totals in the corpus are 7 samples over 25 s of
  /// pinned CPU, then 12, 16, 17, 19 and 31, with top entries of one or two samples
  /// and between 6 and 19 places elided behind them. One reader of such a profile
  /// put it exactly: seven samples with a flat one-apiece distribution conveys no
  /// ranking at all.
  ///
  /// Four, matching \ref MinProfileSamples: below that a count is not evidence
  /// about a place any more than one sample is evidence about a run.
  static constexpr uint64_t MinHotSamples = 4;

  /// Frames of the covering path reported. The innermost are kept: a deep path in
  /// a compiler is mostly the pass manager that every stack ends in, and what
  /// distinguishes this run from any other is at the other end.
  static constexpr size_t MaxUnder = 8;

  /// Samples a run needs before where they landed says anything about it. One
  /// sample is where the program happened to be a fifth of a second in, which for
  /// a program that crashes early is inside the command-line parser: measured on
  /// one such run, 1478 characters reporting `llvm::cl::apply`, which was the
  /// largest section of the response after the backtrace and had nothing to do
  /// with the fault.
  static constexpr uint64_t MinProfileSamples = 4;

  /// The share of samples a function has to appear in to be on that path, as a
  /// reciprocal. Half: a function on the stack for half a run is where the run is,
  /// and a threshold of every sample is broken by one sample taken in the dynamic
  /// loader before the program reached its own code.
  static constexpr uint64_t UnderShareDivisor = 2;
private:
  struct Site {
    std::string Function;
    std::string File;
    uint32_t Line = 0;
    lldb::tid_t Tid = 0;
    uint64_t Samples = 0;

    /// The sample this function was first found in, which breaks ties in a way
    /// that does not depend on how the map was built.
    uint64_t FirstSample = 0;

  };

  /// How many samples a function was anywhere on the stack of, and how deep it sat
  /// in them.
  ///
  /// This is what says where a run is. Self time -- which function the innermost
  /// frame was in -- is the wrong question for an unoptimized build: measured on a
  /// compiler looping inside one analysis, the eight hottest sites were
  /// `isPresent`, `capacity`, `getValueID` and other one-line accessors with one or
  /// two samples each, while the function that was actually spinning appeared in
  /// every sample and in none of them as the innermost frame.
  struct Coverage {
    uint64_t Samples = 0;

    /// Summed so that the mean orders the functions the way a backtrace is
    /// ordered. Two functions on one stack cannot both be innermost, so the means
    /// separate a caller from its callee even where their sample counts agree.
    uint64_t DepthSum = 0;
  };

  /// Keyed by thread and innermost function, because a program with one thread
  /// spinning and eight parked has an answer, and it is which thread.
  std::map<std::pair<lldb::tid_t, std::string>, Site> m_sites;

  std::map<lldb::tid_t, uint64_t> m_per_thread;

  /// Keyed by thread and function, for the same reason the sites are.
  std::map<std::pair<lldb::tid_t, std::string>, Coverage> m_coverage;

  uint64_t m_samples = 0;
};

//===----------------------------------------------------------------------===//
// Result
//===----------------------------------------------------------------------===//

/// What one capture resolved to and what it cost.
struct CaptureReport {
  std::string Expr;

  /// The spelling actually evaluated, when a fixit repaired the one given and
  /// the repair worked. Empty otherwise, including when a fixit was suggested
  /// and the suggestion failed too -- adopting one of those would pin the
  /// capture to a spelling that never resolved.
  ///
  /// Reported because the run is not evaluating what the caller wrote, and a
  /// caller who cannot see that has no way to correlate a value with the
  /// expression that produced it. Stated once here rather than per hit: the
  /// rewrite is decided at the first resolution and holds for the run.
  std::string FixedExpr;

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

  /// Set for a capture the program recorded for itself, which likewise has
  /// neither a tier nor a per-hit cost: no name was resolved and no expression
  /// was run, because the value was copied out where the program held it.
  ///
  /// This is also what says whether the stops an observation's condition still
  /// takes were spent reading values. A capture recorded in the program means
  /// the hit cost nothing at all; one read at a stop means it cost a stop.
  bool InProcess = false;

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

  /// See \ref LocationResolution::SourceNewerThanBinary. Reported beside
  /// \ref ResolvedLocations, because that is the number a reader takes as the
  /// assurance that the tracepoint is where they asked for -- and it says
  /// nothing about whether the line still is.
  std::optional<std::string> SourceNewerThanBinary;

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

  /// Whether this observation's own work was compiled into the program rather
  /// than done at a stop, and why it was not where it was not.
  ///
  /// Said per observation because the answer differs per observation: one
  /// tracepoint's condition may be compiled in while another's function had no
  /// source to recompile. A caller comparing hit counts between runs needs to
  /// know which of them paid for a stop per hit.
  ///
  /// "In-process" is not by itself the claim that nothing stopped. An
  /// observation whose condition is compiled in still stops at every hit the
  /// condition lets through, unless its captures went into the program too;
  /// which of them did is on each capture, where the granularity belongs.
  bool InProcess = false;
  std::string FallbackReason;

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

  /// What each hit saw, in order: the captures of that hit rendered as one
  /// string, which is the same tuple an emission mode compares against the
  /// previous hit.
  ///
  /// Kept for comparing two runs of one plan, where the answer is the hit at
  /// which they stopped agreeing, and never rendered on its own: a run's own
  /// report already says everything these hold, in the aggregate. Bounded, with
  /// the overflow counted, because "the runs agreed" must not be able to mean
  /// "they agreed as far as anything was kept".
  std::vector<std::string> HitTuples;
  uint64_t HitTuplesDropped = 0;

  llvm::json::Value Render() const;
};

/// One capture that could not be read, reported where a caller reads it rather
/// than only as a count beside the expression that failed.
///
/// This is a top-level array on the result and not a field on the capture,
/// because a capture that cannot resolve is a fault in the request and the
/// caller has to see it without going looking. Reached through the aggregate it
/// is a JSON document escaped into a string nested two levels under a label,
/// which is where a reader stops reading.
struct CaptureFailureReport {
  /// The observation the capture belongs to, since an expression alone does not
  /// say where it was being read.
  std::string Label;

  /// The expression as the caller wrote it. What the aggregate is keyed on, so
  /// that this entry and that histogram name the same thing.
  std::string Expr;

  /// The spelling a fixit produced, empty when there was none. Set whether or
  /// not the repair worked; \ref FixApplied is what says which.
  std::string FixedExpr;

  /// Whether \ref FixedExpr resolved and was adopted for the run, as against
  /// having been suggested and failed as well.
  bool FixApplied = false;

  CaptureFailure Kind = CaptureFailure::Situational;

  /// The evaluator's own account, truncated. What the compiler objected to is
  /// more use than any paraphrase of it.
  std::string Reason;

  CaptureCandidates Candidates;

  /// Whether the capture was turned off for the rest of the run.
  bool Disabled = false;

  /// Whether this was the observation's `when` condition rather than one of its
  /// captures. A condition that cannot be evaluated silently records no hits at
  /// all, which looks exactly like code that never ran.
  bool IsCondition = false;

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

  /// The unwinder's index for that frame, which is what \ref Locals and \ref
  /// Source were read from and what a follow-up run has to select to read
  /// anything else there.
  ///
  /// Reported because it is rarely zero and a reader has no way to work it out:
  /// ranking drops the frames that resolved no source, so a fault inside a
  /// library -- a null dereference reached through `strlen` -- names a frame
  /// several hops out from where the unwinder stopped.
  uint32_t FrameIndex = 0;

  /// The thread the frames, locals and source belong to, and how many the
  /// process had. A crash names the thread that faulted and a halt names the
  /// thread that was doing the work, neither of which need be the first.
  lldb::tid_t Tid = LLDB_INVALID_THREAD_ID;
  uint32_t ThreadCount = 0;

  std::vector<RankedFrame> Frames;

  /// Frames the unwinder produced, against which the ranked list is a
  /// reduction.
  uint32_t FramesTotal = 0;

  /// Raw frames not represented in \ref Frames, whether a bound or a ranking
  /// reduction left them out, so that \ref FramesTotal is the sum of this and
  /// the repeat counts of what is reported.
  uint32_t FramesOmitted = 0;

  llvm::json::Object Locals;

  /// Source around \ref Line, when the file could be read.
  std::string Source;

  llvm::json::Value Render() const;
};

/// What a program wrote, with the two streams kept apart.
///
/// Used both for the run's own output and for the text one capture's expression
/// printed, because the shapes are the same and the second is the first read over
/// a narrower window: drain, evaluate, drain again, and what arrived in between
/// belongs to that expression.
struct InferiorOutput {
  std::string Out;
  std::string Err;

  /// Whether a bound dropped anything. Reported rather than left implicit,
  /// because a silently shortened buffer reads as the whole of what was written.
  bool Truncated = false;

  bool Empty() const { return Out.empty() && Err.empty(); }

  /// Appends up to \p Max bytes, dropping from the front of whichever stream
  /// overflows. The front is what goes because the tail says how far the program
  /// got, which is the question this buffer exists to answer.
  void Append(bool Stderr, llvm::StringRef Text, size_t Max);

  /// An object carrying only the streams that produced anything, or null when
  /// neither did. Null rather than an empty object so that a caller can test the
  /// field's presence rather than its contents.
  llvm::json::Value Render() const;
};

/// Marks a capture's value as text the expression printed rather than something
/// it returned. Named here rather than at its two use sites because the aggregate
/// keys on its presence: it is what says the raw `printed` beside the value is a
/// second copy of the answer and not another field.
constexpr llvm::StringLiteral PrintedAsValueField = "printed_as_value";

/// What a capture that printed instead of returning something is aggregated
/// under: the text it wrote, made fit to be a key.
struct PrintedValue {
  /// Empty when what was printed cannot stand as a value, in which case the void
  /// marker with \ref InferiorOutput::Render beside it remains the answer.
  std::string Text;

  /// Set when \ref Text is a prefix of what was printed. \ref Text says so
  /// itself; this is what lets the hit carry a marker as well, since a key
  /// travels into a transition with no room for one beside it.
  bool Shortened = false;
};

/// The value for a capture whose expression ran, returned nothing, and printed.
///
/// Such a capture already reached the aggregate as a document wrapping the void
/// marker, which worked -- transitions and `on_change` did see the text change --
/// and cost fifteen times what the answer does. Measured on a subject printing a
/// sixteen-operand subtree at each of 24 hits, four distinct values: 15.7 kB of
/// response, 96% of it the aggregate, because a thousand-byte document is a key
/// and a key appears once per histogram entry, twice per transition and once per
/// outlier. The text alone is both the answer and a tenth of the bytes.
///
/// Three normalisations, each because without it a value differs from itself.
/// CRLF becomes LF, since standard output is on a terminal and standard error is
/// not, so the same string keyed differently depending on which stream a printer
/// chose. Leading and trailing whitespace goes, because a trailing newline is
/// near-universal in dump output and every key would otherwise differ from its
/// own trimmed form -- and because the program's own unflushed newline can land
/// at the front of a capture's window. What is left of the interior is kept
/// verbatim: a multi-line dump is a multi-line value, and folding its structure
/// away would merge nodes that differ only in an operand list.
///
/// \p MaxChars bounds the key. Past it the text is cut and the cut says how much
/// went and hashes the whole, so that two dumps sharing a prefix are two values
/// rather than one -- a key that under-reported a difference would answer the
/// question this exists to ask.
PrintedValue PrintedAsValue(const InferiorOutput &Printed, size_t MaxChars);

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

  /// Captures and conditions that could not be read, at top level because a
  /// fault in the request is not something a caller should have to find.
  std::vector<CaptureFailureReport> CaptureFailures;

  /// What every capture was observed to hold, over every hit rather than over
  /// the emitted events.
  llvm::json::Value Aggregate = nullptr;

  /// Whether \ref Aggregate covers a prefix of the program rather than the whole
  /// of it, because a ceiling ended the run while it still had work to do.
  ///
  /// Reported because the two are otherwise the same response. A summary of a
  /// prefix is shaped exactly like a summary of a run: a value the program would
  /// have held at a hit that was never reached is missing from `values`, missing
  /// from `outliers`, and indistinguishable there from a value the program never
  /// held at all. Those want opposite responses -- run for longer, or act on the
  /// answer -- and `outliers` is the field a caller is told is usually the answer.
  ///
  /// A ceiling stop only, not any abnormal one. A crash is the program reaching
  /// its own end, so nothing it would have done later is missing from the
  /// aggregate and no ceiling would have changed that.
  ///
  /// Measured on a program making 100,000 calls with one mishandled value at call
  /// 61,804: at a 120-second ceiling the run reached 30,000 of them and reported
  /// an aggregate that read like the whole program's.
  bool AggregateCoversPrefix = false;

  /// A repeating block the end of the run kept traversing. For a program that
  /// did not terminate this is the answer.
  std::optional<CycleReport> Cycle;

  TerminalEvent Terminal;

  std::optional<ArtifactReport> Artifact;

  /// The program's own output, which is often the only evidence of how far it
  /// got.
  ///
  /// The two streams are kept apart rather than appended into one buffer. They
  /// were interleaved with no separator, which loses the one distinction a
  /// reader of a compiler's output needs: a `dump()` goes to stderr and the
  /// compiled result goes to stdout, and a caller looking for what a capture
  /// printed could not tell which half was which. Nothing orders one against the
  /// other -- two pipes drained in turn carry no relative timing -- so a single
  /// buffer was claiming an interleaving it never knew.
  InferiorOutput Output;

  /// Where the program was found while it ran, null when it finished before any
  /// sample was due.
  llvm::json::Value Profile = nullptr;

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

  /// The last of what recording a hit does, once its values are in hand: the
  /// tuple two runs are compared by, the tail a cycle is found over, and the
  /// event the emission mode allows.
  ///
  /// Shared, because a hit the program recorded for itself has to reach the
  /// report by the same route as one the debugger stopped for. Only how the
  /// values were obtained differs, and a report in which that changed the
  /// emission decision or the comparison would make the two modes
  /// incomparable -- which is the one thing this feature must not do.
  ///
  /// \p AtAStop is false for a hit read out of the program's own records. Such a
  /// hit carries no thread and no time of its own, nothing having watched it
  /// happen, so the event leaves those out rather than filling them with the
  /// moment the record was read.
  void FileHit(ObservationSite &Site, uint64_t Seq, llvm::json::Object Values,
               std::string Rendered, StackFrame *Frame, bool AtAStop);

  /// Arms a breakpoint at the return address of \p Ctx's frame, so that leaving
  /// a frame can be observed without unwinding at every hit of something else.
  /// Unwinding per hit to ask who called is what makes the obvious
  /// implementation cost stack depth times hits.
  void ArmReturn(ReturnSet &Set, StoppointCallbackContext *Ctx,
                 BreakpointHitCallback Callback, void *Baton);

  llvm::Error InstallObservations();

  /// Sets the internal breakpoint at which the plan's own work is compiled into
  /// the program.
  ///
  /// Not here, before the launch: a copy cannot be compiled until the dynamic
  /// loader has finished starting up, so the launch stop is too early. Nor at
  /// whatever stop the run happens to take next, because a plan whose
  /// tracepoints never stop the program takes none -- which is the case this
  /// exists for.
  void ArrangeCompilingTracepointsIn();

  /// The internal breakpoint's callback, which compiles what it can and never
  /// reports the stop.
  static bool CompileAtThisStop(void *Baton, StoppointCallbackContext *Ctx,
                                lldb::user_id_t, lldb::user_id_t);

  /// Compiles each observation's condition and captures into the program where
  /// it can, and records why it could not where it could not.
  void CompileTracepointsIntoProcess();

  /// Why \p Site's work cannot be compiled into the program at all, or nothing
  /// when it can.
  ///
  /// Prose rather than a code, because every one of these costs speed and not
  /// correctness: the work is done at a stop instead, and the only thing to do
  /// with the reason is read it.
  std::optional<std::string> WhyNotInProcess(const ObservationSite &Site) const;

  /// Why \p Site's captures have to be read at a stop even though the rest of
  /// its work can be compiled in, or nothing when the program can record them
  /// itself.
  ///
  /// Kept apart from \ref WhyNotInProcess because the two answers are different
  /// sizes. A condition compiled in removes the stops of the hits it excludes;
  /// captures recorded in the program remove the rest, and an observation can
  /// have the first without the second.
  std::optional<std::string>
  WhyCapturesAreReadAtAStop(const ObservationSite &Site) const;

  /// Takes what the injected code has recorded -- the hit counts it keeps and
  /// the values it wrote -- so that a hit the program was not stopped for is
  /// still a hit the report knows about.
  ///
  /// \p RunHasEnded releases the hits whose values are still incomplete. A
  /// thread held still partway through a hit has written some of that hit's
  /// values and not the rest, and while the program is running the rest is
  /// likelier to arrive than not; once it has stopped for good, what is there is
  /// all there will be.
  void TakeWhatTheProgramRecorded(bool RunHasEnded);

  /// Files one hit whose values the program recorded, as \ref RecordHit files
  /// one the debugger read at a stop.
  void RecordCompiledInHit(ObservationSite &Site,
                           llvm::ArrayRef<lldb::ValueObjectSP> Values);

  /// Mirrors \p Site's tracepoint being switched on or off onto its compiled-in
  /// site, which is what gates code the debugger cannot enable and disable.
  void SetCompiledInGate(const ObservationSite &Site, bool Open);

  llvm::Error Launch();
  Outcome WaitForEnd();

  /// Whether the run can afford to stop the program to sample it again.
  ///
  /// Bounded as a share of the run rather than as a fixed interval, for the same
  /// reason an expensive capture is: what a stop costs is a property of the
  /// platform and the program, not something a caller can be asked to know. A
  /// long run is sampled often enough to name a hot function, a short one barely
  /// at all, and neither is slowed by more than the share.
  bool SamplingIsAffordable() const;

  /// Records where every thread of \p P is stopped, for the profile. Called at a
  /// stop the plan did not ask for, which is where the program is under no
  /// obligation to be anywhere in particular -- the property a sample needs.
  void SampleStacks(Process &P);

  void CollectTerminalEvent(Outcome Result);

  /// Reads whatever the program has written since the last drain.
  ///
  /// Unconditional in the run loop, whatever the plan asked for: a full pipe
  /// blocks the program writing to it, so a run that stopped draining would hang
  /// the very program it is observing.
  ///
  /// \p Into names where the text goes. Null is the run's own output. A capture's
  /// buffer is how an expression's printing is attributed to it, and it is only
  /// correct because the drain that brackets the other side ran first: what is
  /// here now arrived while that one expression was running.
  ///
  /// \p Wait is how long to keep looking. Zero takes what has already reached the
  /// debugger and returns, which is what the run loop wants; a capture has to wait,
  /// because the program's pipe is read on another thread and a write that has
  /// returned inside the process is not readable here yet.
  void DrainInferiorOutput(InferiorOutput *Into = nullptr,
                           std::chrono::microseconds Wait =
                               std::chrono::microseconds::zero());

  /// Runs `fflush(0)` in the observed process, so that what an expression printed
  /// through a buffered stream is readable now rather than at some later hit.
  ///
  /// Costs an expression evaluation, so it is spent only where it can change an
  /// answer -- see the call site. Failure is silent: a program with no `fflush`
  /// to call, or one whose call did not run, is a program whose buffered output
  /// arrives late, which is the situation this is trying to improve rather than a
  /// failure of the capture beside it.
  void FlushInferiorOutput();

  void FlushHeldEvents();
  void WriteEvent(ObservationSite &Site, llvm::json::Object Event);

  /// Kills the observed process and drops the target. Runs however the run
  /// ended, including on the paths that never got the program started: a target
  /// left behind holds this run's breakpoints, and in a session somebody else
  /// is using it also outlives the call that made it.
  void Teardown();

  /// The report for \p Label, or null when the plan holds no such observation.
  ObservationReport *ReportFor(llvm::StringRef Label);

  /// Records that the plan saw the program move. The stall ceiling is measured
  /// against this rather than against the event stream, because a mode that
  /// reduces the stream is not the program slowing down.
  void NoteProgress();

  ValueResolutionOptions CaptureOptions() const;

  /// Names \p Expr could have used instead, read out of debug info alone.
  ///
  /// Nothing is compiled and no module is walked for symbols. An unresolved
  /// name is exactly the situation in which a run cannot afford either, and the
  /// point of stopping a stable failure at its first hit is not to spend the
  /// run establishing it. So the sources are the two already to hand: the
  /// frame's own variable list for a name that is not in scope, and the
  /// children of the resolved path prefix for a member the type does not have
  /// -- reached through the frame's path parser rather than through \ref
  /// ResolveValueDWIM, which falls through to the expression evaluator when a
  /// path fails.
  ///
  /// Returns nothing without a frame, which is the case at a return site: the
  /// current frame there belongs to the caller, and its names are not the ones
  /// the capture was reaching for.
  ///
  /// Called once per failing capture rather than once per hit.
  CaptureCandidates CandidatesFor(llvm::StringRef Expr, CaptureFailure Kind,
                                  StackFrame *Frame);

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

  /// The internal breakpoint whose stop the conditions are compiled in at, and
  /// whether that has happened. Asked once: a compile refused at that stop is
  /// refused for a reason the run cannot change.
  lldb::break_id_t m_compile_at = LLDB_INVALID_BREAK_ID;
  bool m_compile_attempted = false;

  /// Whether any observation records its values in the program rather than being
  /// stopped for them, which is what makes the note about what such a hit cannot
  /// carry worth writing.
  bool m_recorded_without_stopping = false;

  /// Values the compiled-in tracepoints recorded that never reached the report.
  /// Accumulated over the run and said once, since every stop reads what has been
  /// recorded since the last one and a note per stop would repeat itself.
  uint64_t m_records_lost = 0;

  Aggregator m_aggregator;
  StackProfile m_profile;

  /// Whether the stop being handled is one this engine asked for in order to
  /// sample. A halt is delivered as a SIGSTOP, which is indistinguishable by stop
  /// reason from a program that died on a signal, so a run that sampled itself
  /// would report itself as crashed on its first sample.
  bool m_halt_for_sample = false;

  /// Wall time this engine has spent stopping the program to sample it, and when
  /// the sample in flight began.
  ///
  /// A halt and a resume are a round trip to the stub each: measured at about
  /// 90 ms per sample on macOS, which on a program that runs freely is time the
  /// program is not running. Left unbounded that inflates the wall clock a run is
  /// judged against, and a program that needed 24 s of a 30 s ceiling would be
  /// reported as having hung -- a false result, which is worse than a coarse
  /// profile.
  std::chrono::microseconds m_sample_cost{0};
  std::chrono::steady_clock::time_point m_sample_started;
  std::unique_ptr<EventArtifact> m_artifact;

  /// A file of this run's own holding the program's standard error, and how much
  /// of it has been read. Empty and invalid when the redirect could not be set
  /// up, in which case stderr stays on the program's terminal and is reported
  /// there -- merged with stdout, and as late as that pipe delivers it.
  ///
  /// See Launch() for why only this stream moves.
  std::string m_stderr_path;
  llvm::sys::fs::file_t m_stderr_reader = llvm::sys::fs::kInvalidFile;
  uint64_t m_stderr_read = 0;

  /// Guards every read of the program's output, and is held across a capture's
  /// whole attribution window. See the capture loop for why: the wait loop and a
  /// breakpoint callback drain from different threads, and an expression
  /// evaluation is what lets the two overlap.
  std::recursive_mutex m_output_mutex;

  /// The hits most recently recorded, each the location it came from joined to
  /// what was read there by \ref CycleEntryValueSeparator, which is what a cycle
  /// is found over. The hit sequence rather than the emitted one, since an
  /// emission mode that drops repeats would destroy the very repetition being
  /// looked for.
  std::vector<std::string> m_tail_entries;

  std::vector<llvm::json::Value> m_tail_events;

  /// Events written so far, which numbers them across all observations so that
  /// one label's event can be placed against another's.
  uint64_t m_seq = 0;

  std::chrono::steady_clock::time_point m_start;

  /// When the program started running, which is what its wall-clock ceiling is
  /// measured from. Equal to \ref m_start until the launch returns, so a failure
  /// before then is still bounded.
  std::chrono::steady_clock::time_point m_running_since;

  /// When the plan last saw the program move, which is the last hit any
  /// observation took rather than the last event written.
  std::chrono::steady_clock::time_point m_last_progress;

  /// Set by a callback that found the run has to end, and read by the wait
  /// loop. A callback is the only place a run that keeps hitting tracepoints
  /// can notice that its time is up.
  std::optional<Outcome> m_requested_end;

  /// What the value renderings met over the whole run: whether a formatter ever
  /// produced a summary, and whether anything was expanded into its members for
  /// want of one. Read once at the end, to say that no formatter matched anything
  /// -- see Run().
  bool m_saw_summary = false;
  bool m_saw_expansion = false;

  ObservationResult m_result;
};

} // namespace lldb_private::mcp

#endif
