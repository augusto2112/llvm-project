//===-- ObservationEngineTest.cpp -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Plugins/Protocol/MCP/ObservationEngine.h"
#include "Plugins/Protocol/MCP/ObservationPlan.h"
#include "lldb/Target/DWIMValueResolution.h"
#include "llvm/Support/JSON.h"
#include "gtest/gtest.h"
#include <chrono>
#include "llvm/ADT/StringRef.h"
#include <utility>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

using namespace lldb_private;
using namespace lldb_private::mcp;

namespace {

using Ms = std::chrono::milliseconds;

EmitDecisionInput Hit(EmitMode Mode, std::optional<std::string> Previous,
                      std::string Current, uint64_t HitIndex) {
  EmitDecisionInput In;
  In.Mode = Mode;
  In.Previous = std::move(Previous);
  In.Current = std::move(Current);
  In.HitIndex = HitIndex;
  return In;
}

/// Runs a whole value sequence through the decision, so that a mode is judged
/// by the stream it produces rather than by one hit in isolation.
std::vector<EmitDecision>
Replay(EmitMode Mode, llvm::ArrayRef<std::string> Values,
       uint32_t SkipFirst = 0, std::optional<uint32_t> OnlyHit = std::nullopt) {
  std::vector<EmitDecision> Decisions;
  std::optional<std::string> Previous;
  for (size_t I = 0; I < Values.size(); ++I) {
    EmitDecisionInput In = Hit(Mode, Previous, Values[I], I + 1);
    In.SkipFirst = SkipFirst;
    In.OnlyHit = OnlyHit;
    Decisions.push_back(DecideEmit(In));
    Previous = Values[I];
  }
  return Decisions;
}

CaptureCostInput Cost(Ms Spent, uint64_t ObservedHits, Ms Remaining,
                      uint64_t Errors = 0, bool AtReturn = false) {
  CaptureCostInput In;
  In.Tier = Errors != 0 ? ValueResolutionTier::Unresolved
                        : ValueResolutionTier::Expression;
  In.Spent = Spent;
  In.ObservedHits = ObservedHits;
  In.TotalHits = ObservedHits;
  In.Remaining = Remaining;
  In.Errors = Errors;
  In.AtReturn = AtReturn;
  In.Expr = "I->getName()";
  return In;
}

RawFrame Frame(std::string Function, std::string File, uint32_t Line = 1,
               uint32_t Index = 0) {
  return RawFrame{std::move(Function), std::move(File), Line, Index};
}

/// A backtrace with the unwinder's own frame indices filled in, which is the only
/// way a real one arrives and what anything read out of a frame afterwards
/// depends on.
std::vector<RawFrame> Backtrace(std::vector<RawFrame> Frames) {
  for (size_t I = 0, E = Frames.size(); I != E; ++I)
    Frames[I].Index = static_cast<uint32_t>(I);
  return Frames;
}

} // namespace

//===----------------------------------------------------------------------===//
// Emission
//===----------------------------------------------------------------------===//

TEST(ObservationEngineTest, EveryHitEmitsEveryHit) {
  EXPECT_EQ(Replay(EmitMode::EveryHit, {"1", "1", "2"}),
            std::vector<EmitDecision>(
                {EmitDecision::Emit, EmitDecision::Emit, EmitDecision::Emit}));
}

TEST(ObservationEngineTest, OnChangeEmitsOnlyTransitions) {
  // The first hit is a change from nothing having been seen, or a capture that
  // never varies would look identical to a location that never ran.
  EXPECT_EQ(Replay(EmitMode::OnChange, {"1", "1", "2", "2", "1"}),
            std::vector<EmitDecision>({EmitDecision::Emit, EmitDecision::Skip,
                                       EmitDecision::Emit, EmitDecision::Skip,
                                       EmitDecision::Emit}));
}

TEST(ObservationEngineTest, OnChangeEmitsOnceForAConstantCapture) {
  EXPECT_EQ(Replay(EmitMode::OnChange, {"false", "false", "false"}),
            std::vector<EmitDecision>(
                {EmitDecision::Emit, EmitDecision::Skip, EmitDecision::Skip}));
}

TEST(ObservationEngineTest, FirstAndLastHoldsTheTailWhenNothingChanged) {
  // The value never changes, and the last hit must still be emitted: a held
  // decision is what makes "last" reachable, since no hit knows it is the last
  // one. Every hit past the first holds, so whichever held most recently is the
  // one flushed when the run ends.
  std::vector<EmitDecision> Decisions =
      Replay(EmitMode::FirstAndLast, {"7", "7", "7", "7"});
  EXPECT_EQ(Decisions, std::vector<EmitDecision>(
                           {EmitDecision::Emit, EmitDecision::Hold,
                            EmitDecision::Hold, EmitDecision::Hold}));
  EXPECT_EQ(Decisions.back(), EmitDecision::Hold);
}

TEST(ObservationEngineTest, FirstAndLastEmitsTheOnlyHitOnce) {
  // One hit is both the first and the last, and must not be emitted twice.
  EXPECT_EQ(Replay(EmitMode::FirstAndLast, {"7"}),
            std::vector<EmitDecision>({EmitDecision::Emit}));
}

TEST(ObservationEngineTest, OnlyHitSelectsOneHitAndOverridesTheMode) {
  EXPECT_EQ(
      Replay(EmitMode::OnChange, {"1", "1", "1", "1"}, /*SkipFirst=*/0,
             /*OnlyHit=*/3),
      std::vector<EmitDecision>({EmitDecision::Skip, EmitDecision::Skip,
                                 EmitDecision::Emit, EmitDecision::Skip}));
}

TEST(ObservationEngineTest, OnlyHitIsNumberedPastTheSkippedHits) {
  // A hit number is one the caller counted in the program, so skipping shifts
  // which recorded hit carries it rather than renumbering from the first
  // survivor.
  EXPECT_EQ(Replay(EmitMode::EveryHit, {"a", "b", "c"}, /*SkipFirst=*/2,
                   /*OnlyHit=*/3),
            std::vector<EmitDecision>(
                {EmitDecision::Emit, EmitDecision::Skip, EmitDecision::Skip}));
}

TEST(ObservationEngineTest, OnlyHitBeyondTheHitsEmitsNothing) {
  EXPECT_EQ(
      Replay(EmitMode::EveryHit, {"a", "b"}, /*SkipFirst=*/0,
             /*OnlyHit=*/9),
      std::vector<EmitDecision>({EmitDecision::Skip, EmitDecision::Skip}));
}

//===----------------------------------------------------------------------===//
// Expression cost control
//===----------------------------------------------------------------------===//

TEST(ObservationEngineTest, CostControlKeepsACaptureInsideItsShare) {
  // Spending exactly a quarter of what is left is inside the budget; the
  // boundary belongs to the capture.
  CaptureCostDecision Kept = AssessCaptureCost(Cost(Ms(250), 10, Ms(1000)));
  EXPECT_FALSE(Kept.Disable);
  EXPECT_TRUE(Kept.Note.empty());
  EXPECT_DOUBLE_EQ(Kept.PerHitMs, 25.0);
}

TEST(ObservationEngineTest, CostControlDisablesACaptureOverItsShare) {
  CaptureCostDecision Off = AssessCaptureCost(Cost(Ms(251), 10, Ms(1000)));
  EXPECT_TRUE(Off.Disable);
  EXPECT_EQ(Off.ObservedHits, 10u);
  EXPECT_EQ(Off.TotalHits, 10u);
  EXPECT_DOUBLE_EQ(Off.ProjectedMs, 251.0);
  EXPECT_FALSE(Off.Note.empty());
}

TEST(ObservationEngineTest, CostControlNamesTheCheaperPathForm) {
  CaptureCostDecision Off = AssessCaptureCost(Cost(Ms(900), 3, Ms(1000)));
  ASSERT_TRUE(Off.Disable);
  // A note that says only "too slow" leaves the caller to guess the fix.
  EXPECT_NE(Off.Note.find("I->Name"), std::string::npos);
}

TEST(ObservationEngineTest, CostControlNeverDisablesAPath) {
  // A path is a debug-info lookup and a memory read; no measurement makes
  // turning it off a saving worth the data.
  CaptureCostInput In = Cost(Ms(10000), 5, Ms(10));
  In.Tier = ValueResolutionTier::VariablePath;
  CaptureCostDecision Kept = AssessCaptureCost(In);
  EXPECT_FALSE(Kept.Disable);
  EXPECT_TRUE(Kept.Note.empty());
  // The measurement is still reported, so an expensive path is visible.
  EXPECT_DOUBLE_EQ(Kept.PerHitMs, 2000.0);
}

// A name that resolves nowhere costs an expression compile at every hit and
// yields nothing. Measured at ~90 ms each, so waiting for the time budget to
// notice wastes seconds of a timeout-bound run and then blames the spelling.
TEST(ObservationEngineTest, CaptureFailingAtEveryHitIsStoppedAsUnresolvable) {
  // Well inside the time budget: this must not depend on the cost test.
  CaptureCostInput In = Cost(Ms(1), UnresolvableCaptureAttempts, Ms(100000));
  In.Tier = ValueResolutionTier::Unresolved;
  In.Errors = UnresolvableCaptureAttempts;
  In.Expr = "Low16";

  CaptureCostDecision Off = AssessCaptureCost(In);
  ASSERT_TRUE(Off.Disable);
  EXPECT_NE(Off.Note.find("does not resolve"), std::string::npos) << Off.Note;
  // The cost advice would be nonsense for a bare identifier, which is already
  // the cheapest form a name has.
  EXPECT_EQ(Off.Note.find("cheaper"), std::string::npos) << Off.Note;
}

TEST(ObservationEngineTest, ACaptureThatSometimesResolvesIsKept) {
  // A pointer null at the first hits and set later is worth keeping, so only a
  // capture that failed at *every* attempt is treated as unresolvable.
  CaptureCostInput In = Cost(Ms(1), 10, Ms(100000));
  In.Tier = ValueResolutionTier::Expression;
  In.Errors = 9;

  EXPECT_FALSE(AssessCaptureCost(In).Disable);
}

TEST(ObservationEngineTest, OneFailureIsNotEnoughToCallANameUnresolvable) {
  CaptureCostInput In = Cost(Ms(1), 1, Ms(100000));
  In.Tier = ValueResolutionTier::Unresolved;
  In.Errors = 1;

  EXPECT_FALSE(AssessCaptureCost(In).Disable);
}

TEST(ObservationEngineTest, CostControlSurvivesAnExhaustedBudget) {
  CaptureCostDecision Off = AssessCaptureCost(Cost(Ms(1), 1, Ms(0)));
  EXPECT_TRUE(Off.Disable);
  EXPECT_DOUBLE_EQ(Off.RemainingMs, 0.0);
}

TEST(ObservationEngineTest, CostControlSaysNothingWithoutAMeasurement) {
  CaptureCostDecision Unknown = AssessCaptureCost(Cost(Ms(0), 0, Ms(0)));
  EXPECT_FALSE(Unknown.Disable);
  EXPECT_DOUBLE_EQ(Unknown.PerHitMs, 0.0);
}

TEST(ObservationEngineTest, PathFormOfAGetter) {
  EXPECT_EQ(SuggestPathForm("I->getName()"),
            std::optional<std::string>("I->Name"));
  EXPECT_EQ(SuggestPathForm("a.b.getFoo()"),
            std::optional<std::string>("a.b.Foo"));
  EXPECT_EQ(SuggestPathForm("getCount()"), std::optional<std::string>("Count"));
}

TEST(ObservationEngineTest, PathFormRejectsWhatIsNotAGetter) {
  // A call with arguments is computing something no member holds, and a bare
  // `get` names nothing.
  EXPECT_FALSE(SuggestPathForm("I->getOperand(0)"));
  EXPECT_FALSE(SuggestPathForm("f()"));
  EXPECT_FALSE(SuggestPathForm("p->get()"));
  EXPECT_FALSE(SuggestPathForm("I->getname()"));
  EXPECT_FALSE(SuggestPathForm("count"));
  EXPECT_FALSE(SuggestPathForm(""));
}

//===----------------------------------------------------------------------===//
// Frame arming
//===----------------------------------------------------------------------===//

namespace {

/// Stack addresses for the tests below. The stack grows down, so an outer frame
/// has the higher call-frame address, and the names are ordered to match.
constexpr lldb::addr_t kOuter = 3000;
constexpr lldb::addr_t kMiddle = 2000;
constexpr lldb::addr_t kInner = 1000;

/// Return addresses, which say nothing about which frame is returning: that is
/// the whole reason a frame is not identified by one.
constexpr lldb::addr_t kRetA = 0x4000;
constexpr lldb::addr_t kRetB = 0x5000;

constexpr lldb::tid_t kThreadOne = 11;
constexpr lldb::tid_t kThreadTwo = 22;

} // namespace

TEST(ObservationEngineTest, ArmedFrameReturnsOnce) {
  ArmedFrames Armed;
  Armed.Arm(kThreadOne, kRetA, kMiddle);
  EXPECT_FALSE(Armed.Empty());

  EXPECT_TRUE(Armed.Returned(kThreadOne, kRetA, kOuter));
  EXPECT_TRUE(Armed.Empty());
  EXPECT_EQ(Armed.GetAbandoned(), 0u);

  // The breakpoint at the return address stays behind and is reached by every
  // later call from the same call site. Nothing is armed there now, so none of
  // those is a return anybody asked about.
  EXPECT_FALSE(Armed.Returned(kThreadOne, kRetA, kOuter));
}

TEST(ObservationEngineTest, AnotherThreadsReturnIsNotThisFrames) {
  ArmedFrames Armed;
  Armed.Arm(kThreadOne, kRetA, kMiddle);

  // Both threads run through the same return address. Counting arrivals rather
  // than frames would report this one as the armed frame returning, and then
  // leave the real return unreported.
  EXPECT_FALSE(Armed.Returned(kThreadTwo, kRetA, kOuter));
  EXPECT_FALSE(Armed.Empty());

  EXPECT_TRUE(Armed.Returned(kThreadOne, kRetA, kOuter));
  EXPECT_TRUE(Armed.Empty());
}

TEST(ObservationEngineTest, RecursionReturnsInnermostFirst) {
  ArmedFrames Armed;
  Armed.Arm(kThreadOne, kRetA, kOuter);
  Armed.Arm(kThreadOne, kRetB, kMiddle);

  // The inner frame returns into the outer one, so the thread is now standing
  // at exactly the outer frame's call-frame address. Reading that as "at or
  // below, therefore gone" would throw the outer frame away here and then
  // report its real return as belonging to nothing.
  EXPECT_TRUE(Armed.Returned(kThreadOne, kRetB, kOuter));
  EXPECT_FALSE(Armed.Empty());
  EXPECT_EQ(Armed.GetAbandoned(), 0u);

  EXPECT_TRUE(Armed.Returned(kThreadOne, kRetA, kOuter + 1000));
  EXPECT_TRUE(Armed.Empty());
  EXPECT_EQ(Armed.GetAbandoned(), 0u);
}

TEST(ObservationEngineTest, AStopInsideAnArmedFrameIsNotItsReturn) {
  ArmedFrames Armed;
  Armed.Arm(kThreadOne, kRetA, kMiddle);

  // Stopped at the armed address, but deeper than the armed frame, so the frame
  // is still on the stack and this is not it returning.
  EXPECT_FALSE(Armed.Returned(kThreadOne, kRetA, kInner));
  EXPECT_FALSE(Armed.Empty());
  EXPECT_EQ(Armed.GetAbandoned(), 0u);
}

TEST(ObservationEngineTest, AFrameLeftWithoutReturningIsDiscardedNotReported) {
  ArmedFrames Armed;
  Armed.Arm(kThreadOne, kRetA, kMiddle);

  // An exception or a longjmp takes the frame away without it ever reaching the
  // return address, and the same call site calls again.
  Armed.Arm(kThreadOne, kRetA, kMiddle);

  // Waiting on the address alone would have reported this one return twice:
  // once for the frame that never came back, and once for the frame that did.
  EXPECT_TRUE(Armed.Returned(kThreadOne, kRetA, kOuter));
  EXPECT_TRUE(Armed.Empty());
  EXPECT_EQ(Armed.GetAbandoned(), 1u);
}

TEST(ObservationEngineTest, AnUnwindPastSeveralFramesAbandonsThemAll) {
  ArmedFrames Armed;
  Armed.Arm(kThreadOne, kRetA, kMiddle);
  Armed.Arm(kThreadOne, kRetB, kInner);

  // A handler outside both of them is where an exception lands, and being there
  // is proof that neither frame is still on the stack.
  EXPECT_FALSE(Armed.EnclosesFrame(kThreadOne, kOuter));
  EXPECT_TRUE(Armed.Empty());
  EXPECT_EQ(Armed.GetAbandoned(), 2u);
}

TEST(ObservationEngineTest, AGateEnclosesOnlyTheThreadInsideIt) {
  ArmedFrames Armed;
  Armed.Arm(kThreadOne, kRetA, kOuter);

  // Being called from a function is having one of its frames still below this
  // one, on this thread. A gate remembered as a state change instead would be
  // open for every thread at once.
  EXPECT_TRUE(Armed.EnclosesFrame(kThreadOne, kInner));
  EXPECT_FALSE(Armed.EnclosesFrame(kThreadTwo, kInner));

  // Still open: asking about another thread must not disarm anything.
  EXPECT_TRUE(Armed.EnclosesFrame(kThreadOne, kInner));
}

TEST(ObservationEngineTest, AFrameDoesNotEncloseItself) {
  ArmedFrames Armed;
  Armed.Arm(kThreadOne, kRetA, kMiddle);

  // The frame the gate armed and the frame being asked about are the same one,
  // which is what observing a function as called from itself does at the
  // outermost call. Nothing called that one, so the gate is shut for it.
  EXPECT_FALSE(Armed.EnclosesFrame(kThreadOne, kMiddle));

  // Shut, not gone: the recursive calls below it are the ones it does cover.
  EXPECT_FALSE(Armed.Empty());
  EXPECT_TRUE(Armed.EnclosesFrame(kThreadOne, kInner));
}

TEST(ObservationEngineTest, EveryThreadHasToLeaveBeforeTheSetIsEmpty) {
  ArmedFrames Armed;
  Armed.Arm(kThreadOne, kRetA, kMiddle);
  Armed.Arm(kThreadTwo, kRetA, kMiddle);

  EXPECT_TRUE(Armed.Returned(kThreadOne, kRetA, kOuter));
  // The breakpoints behind the set are shared, so switching them off on the
  // first thread's return would stop watching the second thread's frame.
  EXPECT_FALSE(Armed.Empty());

  EXPECT_TRUE(Armed.Returned(kThreadTwo, kRetA, kOuter));
  EXPECT_TRUE(Armed.Empty());
}

//===----------------------------------------------------------------------===//
// Frame ranking
//===----------------------------------------------------------------------===//

TEST(ObservationEngineTest, RankingCollapsesRecursion) {
  // A runaway recursion arrives as one frame repeated until the unwinder gives
  // up; the count is the information, the repetition is not.
  std::vector<RawFrame> Raw;
  for (int I = 0; I < 200; ++I)
    Raw.push_back(Frame("descend", "/src/tree.cpp", 42));
  Raw.push_back(Frame("main", "/src/main.cpp", 7));

  std::vector<RankedFrame> Ranked = RankFrames(Raw);
  ASSERT_EQ(Ranked.size(), 2u);
  EXPECT_EQ(Ranked[0].Function, "descend");
  EXPECT_EQ(Ranked[0].Repeats, 200u);
  EXPECT_EQ(Ranked[1].Function, "main");
  EXPECT_EQ(Ranked[1].Repeats, 1u);
}

TEST(ObservationEngineTest, RankingOnlyCollapsesAdjacentFrames) {
  // Mutual recursion is not one frame repeated, and folding it would claim a
  // depth the stack does not have.
  std::vector<RankedFrame> Ranked =
      RankFrames({Frame("f", "/src/a.cpp"), Frame("g", "/src/a.cpp"),
                  Frame("f", "/src/a.cpp")});
  ASSERT_EQ(Ranked.size(), 3u);
  EXPECT_EQ(Ranked[0].Repeats, 1u);
  EXPECT_EQ(Ranked[1].Function, "g");
  EXPECT_EQ(Ranked[2].Function, "f");
}

TEST(ObservationEngineTest, RankingDemotesSystemFrames) {
  std::vector<RankedFrame> Ranked = RankFrames(
      {Frame("__pthread_kill", "/usr/lib/system/libsystem_kernel.dylib.c"),
       Frame("abort", "/usr/lib/libc.c"),
       Frame("llvm::report_fatal_error", "/src/llvm/lib/Support/Error.cpp", 12),
       Frame("MyPass::run", "/src/mypass/Pass.cpp", 88)});

  ASSERT_EQ(Ranked.size(), 4u);
  // The program's own frames come first and keep their innermost-first order.
  EXPECT_EQ(Ranked[0].Function, "llvm::report_fatal_error");
  EXPECT_EQ(Ranked[1].Function, "MyPass::run");
  EXPECT_FALSE(Ranked[0].IsSystem);
  EXPECT_TRUE(Ranked[2].IsSystem);
  EXPECT_TRUE(Ranked[3].IsSystem);
  // Demotion preserves order within the system frames too.
  EXPECT_EQ(Ranked[2].Function, "__pthread_kill");
}

TEST(ObservationEngineTest, RankingDropsFramesWithNoSource) {
  std::vector<RankedFrame> Ranked =
      RankFrames({Frame("stub", ""), Frame("MyPass::run", "/src/Pass.cpp", 3),
                  Frame("thunk", "")});
  ASSERT_EQ(Ranked.size(), 1u);
  EXPECT_EQ(Ranked[0].Function, "MyPass::run");
}

TEST(ObservationEngineTest, RankingKeepsSourcelessFramesWhenThatIsAllThereIs) {
  // Dropping every frame would report no location at all for a stripped binary,
  // which is the empty result the design exists to avoid.
  std::vector<RankedFrame> Ranked =
      RankFrames({Frame("stub", ""), Frame("thunk", "")});
  ASSERT_EQ(Ranked.size(), 2u);
  EXPECT_EQ(Ranked[0].Function, "stub");
}

TEST(ObservationEngineTest, RankingOfNothingIsNothing) {
  EXPECT_TRUE(RankFrames({}).empty());
}

TEST(ObservationEngineTest, RankingCarriesTheUnwinderIndexOfEachFrame) {
  // Everything read out of a frame afterwards -- its locals, its source listing,
  // a `frame select` in a follow-up run -- is addressed by the unwinder's index,
  // and ranking folds, drops and reorders, so a position in the ranked list says
  // nothing about a position on the stack. Measured on a null dereference reached
  // through `strlen`: frame 0 is `_platform_strlen` with no source and no
  // variables, and the frame the report names is frame 1.
  std::vector<RankedFrame> Ranked = RankFrames(
      Backtrace({Frame("_platform_strlen", ""), Frame("sum_labels", "/s/a.c", 29),
                 Frame("score", "/s/a.c", 35)}));
  ASSERT_EQ(Ranked.size(), 2u);
  EXPECT_EQ(Ranked[0].Function, "sum_labels");
  EXPECT_EQ(Ranked[0].Index, 1u);
  EXPECT_EQ(Ranked[1].Index, 2u);
}

TEST(ObservationEngineTest, AFoldedRunIsIndexedWhereItsLocationCameFrom) {
  // A run of one function keeps the location nearest the failure, so the index
  // has to follow the location: read from the run's outermost frame instead, the
  // locals would belong to a different call than the line the entry reports.
  std::vector<RankedFrame> Ranked = RankFrames(Backtrace(
      {Frame("thunk", ""), Frame("recurse", ""), Frame("recurse", "/s/a.c", 7),
       Frame("main", "/s/a.c", 40)}));
  ASSERT_EQ(Ranked.size(), 2u);
  EXPECT_EQ(Ranked[0].Function, "recurse");
  EXPECT_EQ(Ranked[0].Repeats, 2u);
  EXPECT_EQ(Ranked[0].Line, 7u);
  EXPECT_EQ(Ranked[0].Index, 2u);
}

TEST(ObservationEngineTest, RankingCountsTheSourcelessFramesItDropped) {
  // A shorter list is not a quieter one. Reported against `frames_total`, a
  // reduction that counts nothing reads as one that never happened: measured on a
  // crash reporting `frames_total: 5` beside three frames and no omission at all.
  uint32_t Dropped = 99;
  std::vector<RankedFrame> Ranked = RankFrames(
      Backtrace({Frame("stub", ""), Frame("MyPass::run", "/src/Pass.cpp", 3),
                 Frame("thunk", "")}),
      &Dropped);
  ASSERT_EQ(Ranked.size(), 1u);
  EXPECT_EQ(Dropped, 2u);

  // A run of identical sourceless frames counts once per raw frame, so that the
  // total a caller checks against is a count of frames throughout.
  Dropped = 99;
  RankFrames(Backtrace({Frame("thunk", ""), Frame("thunk", ""),
                        Frame("thunk", ""), Frame("f", "/src/a.cpp", 1)}),
             &Dropped);
  EXPECT_EQ(Dropped, 3u);

  // Nothing was dropped when nothing had source: the frames were kept instead.
  Dropped = 99;
  RankFrames(Backtrace({Frame("stub", ""), Frame("thunk", "")}), &Dropped);
  EXPECT_EQ(Dropped, 0u);
}

TEST(ObservationEngineTest, LocalsAreReadScalarsFirstAndThisLast) {
  // Declaration order hands a shared budget to `this` and the parameters, which
  // for a C++ member function is what the compiler emits first and never what was
  // asked about. Measured on a compiler stopped inside a pass: those four took
  // the whole of a 96-node budget and all eleven body locals came back as elision
  // markers.
  const unsigned Scalar =
      TerminalLocalRank("Worklist", /*IsScalar=*/true, /*IsArgument=*/false);
  const unsigned BodyAggregate =
      TerminalLocalRank("PhiNodes", /*IsScalar=*/false, /*IsArgument=*/false);
  const unsigned Parameter =
      TerminalLocalRank("TM", /*IsScalar=*/false, /*IsArgument=*/true);
  const unsigned This =
      TerminalLocalRank("this", /*IsScalar=*/false, /*IsArgument=*/true);
  const unsigned Temporary =
      TerminalLocalRank("__range1", /*IsScalar=*/false, /*IsArgument=*/false);

  EXPECT_LT(Scalar, BodyAggregate);
  EXPECT_LT(BodyAggregate, Parameter);
  EXPECT_LT(Parameter, This);
  EXPECT_LT(This, Temporary);

  // A scalar parameter is still a scalar: one node and about twenty-five bytes,
  // and for a hang it is as likely to be the loop-carried value as any body
  // local.
  EXPECT_EQ(TerminalLocalRank("N", /*IsScalar=*/true, /*IsArgument=*/true),
            Scalar);

  // The temporary test comes first, so a range-for's own iterator goes last
  // whatever its type says. Eight of thirty-two slots in the frame measured were
  // these, all starved, and none names anything a reader would capture.
  EXPECT_EQ(TerminalLocalRank("__begin2", /*IsScalar=*/true,
                              /*IsArgument=*/false),
            Temporary);
}

TEST(ObservationEngineTest, StarvedLocalsAreOneMarkerThatNamesThem) {
  // Twenty-five copies of `{"_elided":"node budget"}` was 901 characters
  // announcing absence, and a name is the only part of that a caller can act on:
  // it is what goes in the next plan's `capture`.
  const std::string Note = ElidedLocals(
      {"Worklist", "PhiNodes", "ConvertTy"}, /*NotRead=*/0);
  EXPECT_NE(Note.find("3 locals"), std::string::npos) << Note;
  EXPECT_NE(Note.find("Worklist"), std::string::npos) << Note;
  EXPECT_NE(Note.find("ConvertTy"), std::string::npos) << Note;
  // A tenth of what one marker per local cost, names included.
  EXPECT_LT(Note.size(), 3u * 26u);
}

TEST(ObservationEngineTest, TheNamesInThatMarkerAreBoundedAndTheCountIsNot) {
  const std::vector<llvm::StringRef> Many = {"a1", "a2", "a3", "a4", "a5",
                                            "a6", "a7", "a8", "a9", "a10"};
  const std::string Note = ElidedLocals(Many, /*NotRead=*/0);
  // The count covers all of them even where the list does not, so eight names do
  // not read as everything that was left out.
  EXPECT_NE(Note.find("10 locals"), std::string::npos) << Note;
  EXPECT_NE(Note.find("a8"), std::string::npos) << Note;
  EXPECT_EQ(Note.find("a9"), std::string::npos) << Note;
  EXPECT_NE(Note.find("..."), std::string::npos) << Note;
}

TEST(ObservationEngineTest, LocalsNeverReadAreCountedNotNamed) {
  // Past the bound on how many locals are read at all, nothing has read them, so
  // there is no name to offer -- only the count, which is what says the list is a
  // selection.
  EXPECT_EQ(ElidedLocals({}, /*NotRead=*/9), "9 more locals not read");

  // Both at once, in one marker, because they are one fact about the same set.
  const std::string Both = ElidedLocals({"V"}, /*NotRead=*/4);
  EXPECT_NE(Both.find("V"), std::string::npos) << Both;
  EXPECT_NE(Both.find("4 more"), std::string::npos) << Both;

  // Nothing left out means no marker, rather than a marker saying zero.
  EXPECT_TRUE(ElidedLocals({}, /*NotRead=*/0).empty());
}

TEST(ObservationEngineTest, SystemPathsAreJudgedByRoot) {
  EXPECT_TRUE(IsSystemSourcePath("/usr/include/stdio.h"));
  EXPECT_TRUE(IsSystemSourcePath("/opt/homebrew/include/foo.h"));
  EXPECT_TRUE(IsSystemSourcePath("/tmp/tc/lib/gcc/x86_64/14/include/x.h"));
  EXPECT_FALSE(IsSystemSourcePath("/Users/me/src/llvm/lib/IR/Function.cpp"));
  // A user's own Library directory is not the system one.
  EXPECT_FALSE(IsSystemSourcePath("/Users/me/Library/proj/main.cpp"));
  EXPECT_FALSE(IsSystemSourcePath(""));
}

//===----------------------------------------------------------------------===//
// Result rendering
//===----------------------------------------------------------------------===//

namespace {

std::string Render(const llvm::json::Value &V) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  OS << V;
  return S;
}

} // namespace

TEST(ObservationEngineTest, ResultAlwaysCarriesAnOutcomeAndATerminalEvent) {
  // A run with no observations is crash triage, and must not render as an empty
  // document.
  ObservationResult Result;
  Result.Result = Outcome::Crashed;
  Result.Terminal.Description = "signal SIGSEGV";
  Result.Terminal.Function = "MyPass::run";

  std::string S = Render(Result.Render());
  EXPECT_NE(S.find("\"outcome\":\"crashed\""), std::string::npos);
  EXPECT_NE(S.find("signal SIGSEGV"), std::string::npos);
  EXPECT_NE(S.find("MyPass::run"), std::string::npos);
}

TEST(ObservationEngineTest, TerminalNamesTheFrameItsLocalsWereReadFrom) {
  // The trio names a frame that is generally not frame zero, and everything else
  // under the key was read there. Without the index a reader cannot select it in
  // a follow-up run, and cannot tell that the locals belong to the frame the trio
  // names rather than to the innermost one.
  TerminalEvent Terminal;
  Terminal.Description = "EXC_BAD_ACCESS";
  Terminal.Function = "sum_labels";
  Terminal.File = "/s/a.c";
  Terminal.Line = 29;
  Terminal.FrameIndex = 1;

  const llvm::json::Value Rendered = Terminal.Render();
  EXPECT_EQ(Rendered.getAsObject()->getInteger("frame"),
            std::optional<int64_t>(1));

  // Frame zero is the case a reader assumes, so saying so is bytes spent on
  // nothing.
  Terminal.FrameIndex = 0;
  EXPECT_EQ(Terminal.Render().getAsObject()->get("frame"), nullptr);
}

TEST(ObservationEngineTest, TerminalNamesTheThreadOnlyWhenThereWereSeveral) {
  // A crash names the thread that faulted and a halt names the thread that was
  // working, neither of which need be the first one; with several threads running,
  // which one the frames belong to is a choice the report made.
  TerminalEvent Terminal;
  Terminal.Description = "the program was still running";
  Terminal.Tid = 4211;
  Terminal.ThreadCount = 3;
  EXPECT_EQ(Terminal.Render().getAsObject()->getInteger("tid"),
            std::optional<int64_t>(4211));

  Terminal.ThreadCount = 1;
  EXPECT_EQ(Terminal.Render().getAsObject()->get("tid"), nullptr);
}

TEST(ObservationEngineTest, TerminalNamesTheDirectoryItsFramesShareOnce) {
  // Measured on a compiler backtrace of 14 frames over 7 files: the `file` keys
  // and values were 1,478 of the 3,207 characters the list cost, and a
  // 65-character build-directory prefix appeared in every one of them.
  TerminalEvent Terminal;
  Terminal.Description = "EXC_BAD_ACCESS";
  Terminal.File = "/Users/me/build/src/llvm/lib/CodeGen/CodeGenPrepare.cpp";
  Terminal.Line = 7052;
  Terminal.Frames = {
      RankedFrame{"optimizePhiType",
                  "/Users/me/build/src/llvm/lib/CodeGen/CodeGenPrepare.cpp",
                  7052},
      RankedFrame{"PassManager::run",
                  "/Users/me/build/src/llvm/include/llvm/IR/PassManagerImpl.h",
                  76}};

  const llvm::json::Value Value = Terminal.Render();
  const llvm::json::Object *O = Value.getAsObject();
  EXPECT_EQ(O->getString("file_root"),
            std::optional<llvm::StringRef>("/Users/me/build/src/llvm"));
  EXPECT_EQ(O->getString("file"),
            std::optional<llvm::StringRef>("lib/CodeGen/CodeGenPrepare.cpp"));
  EXPECT_EQ((*O->getArray("frames"))[1].getAsObject()->getString("file"),
            std::optional<llvm::StringRef>("include/llvm/IR/PassManagerImpl.h"));

  // Never expressed by leaving `file` out: an absent file already means the frame
  // resolved no source, and a reader cannot be made to read it two ways.
  for (const llvm::json::Value &Frame : *O->getArray("frames"))
    EXPECT_NE(Frame.getAsObject()->get("file"), nullptr);
}

TEST(ObservationEngineTest, ASharedRootIsOnlyNamedWhenItPays) {
  // The field costs its own key and the root's own length, so it has to remove
  // more than it adds: one frame shares a prefix with nothing, and a short prefix
  // is not worth a term.
  TerminalEvent Terminal;
  Terminal.Description = "EXC_BAD_ACCESS";
  Terminal.File = "/Users/me/build/src/llvm/lib/CodeGen/CodeGenPrepare.cpp";
  Terminal.Frames = {RankedFrame{
      "optimizePhiType", "/Users/me/build/src/llvm/lib/CodeGen/CodeGenPrepare.cpp",
      7052}};
  EXPECT_EQ(Terminal.Render().getAsObject()->get("file_root"), nullptr);

  Terminal.File = "/a/x.c";
  Terminal.Frames = {RankedFrame{"f", "/a/x.c", 1}, RankedFrame{"g", "/a/y.c", 2}};
  const llvm::json::Value Value = Terminal.Render();
  const llvm::json::Object *O = Value.getAsObject();
  EXPECT_EQ(O->get("file_root"), nullptr);
  EXPECT_EQ(O->getString("file"), std::optional<llvm::StringRef>("/a/x.c"));
}

TEST(ObservationEngineTest, ASharedRootNeverSplitsADirectoryName) {
  // `slot-3` and `slot-30` share six characters and no directory at all, and a
  // reader joining that prefix onto a relative path would name a file that is not
  // there.
  TerminalEvent Terminal;
  Terminal.Description = "EXC_BAD_ACCESS";
  Terminal.Frames = {
      RankedFrame{"f", "/Users/me/experiment/work/slot-3/src/a.cpp", 1},
      RankedFrame{"g", "/Users/me/experiment/work/slot-30/src/b.cpp", 2}};
  EXPECT_EQ(Terminal.Render().getAsObject()->getString("file_root"),
            std::optional<llvm::StringRef>("/Users/me/experiment/work"));
}

TEST(ObservationEngineTest, TerminalFrameTotalIsWhatItShowsPlusWhatItLeftOut) {
  // `frames_total` is the number the shorter list is checked against, so every
  // reduction has to be counted into the omission or the two cannot be made to
  // meet.
  TerminalEvent Terminal;
  Terminal.Description = "EXC_BAD_ACCESS";
  Terminal.FramesTotal = 9;
  Terminal.FramesOmitted = 4;
  Terminal.Frames = {RankedFrame{"f", "/a/x.c", 1, /*Index=*/0, /*Repeats=*/3},
                     RankedFrame{"g", "/a/x.c", 2, /*Index=*/3, /*Repeats=*/2}};

  const llvm::json::Value Value = Terminal.Render();
  const llvm::json::Object *O = Value.getAsObject();
  int64_t Shown = 0;
  for (const llvm::json::Value &Frame : *O->getArray("frames"))
    Shown += Frame.getAsObject()->getInteger("repeats").value_or(1);
  EXPECT_EQ(Shown + O->getInteger("frames_omitted").value_or(0),
            O->getInteger("frames_total").value_or(0));
}

TEST(ObservationEngineTest, ReportKeepsTheThreeHitCountsApart) {
  ObservationReport Report;
  Report.Label = "visit";
  Report.At = "visit";
  Report.ResolvedLocations = 1;
  Report.Hits = 4012;
  Report.HasCondition = true;
  Report.ConditionTrue = 0;
  Report.ConditionErrors = 4012;

  // "the code never ran", "my condition never fired" and "my condition is not
  // valid" have to be three different readings of the report.
  std::string S = Render(Report.Render());
  EXPECT_NE(S.find("\"hits\":4012"), std::string::npos);
  EXPECT_NE(S.find("\"condition_true\":0"), std::string::npos);
  EXPECT_NE(S.find("\"condition_errors\":4012"), std::string::npos);
}

TEST(ObservationEngineTest, ReportOmitsConditionCountsWithoutACondition) {
  ObservationReport Report;
  Report.Hits = 12;
  std::string S = Render(Report.Render());
  EXPECT_EQ(S.find("condition_true"), std::string::npos);
}

TEST(ObservationEngineTest, ReportKeepsAnUnresolvedLocationVisible) {
  ObservationReport Report;
  Report.At = "vist";
  Report.ResolvedLocations = 0;
  Report.ResolutionError = "no code matched \"vist\". Did you mean \"visit\"?";

  std::string S = Render(Report.Render());
  EXPECT_NE(S.find("\"resolved_locations\":0"), std::string::npos);
  EXPECT_NE(S.find("Did you mean"), std::string::npos);
}

TEST(ObservationEngineTest, PathCaptureCollapsesToItsTierAndItsReadCount) {
  // The tier says how it resolved; the count says it then read, and at how many
  // hits. Without the count the row states nothing positive at all and a reader
  // has to infer "read at every hit" from the absence of an `errors` field --
  // which is exactly the reading a silent failure defeats.
  CaptureReport Capture;
  Capture.Expr = "i";
  Capture.Tier = ValueResolutionTier::VariablePath;
  Capture.Evaluations = 4012;
  EXPECT_EQ(Render(Capture.Render()), "\"path x4012\"");
}

// A capture the run never reached says nothing about whether it could be read.
// Rendering the default tier here would label it "unavailable", which is the
// word a capture that was evaluated and failed gets, so a plan whose tracepoint
// never fired would read as a plan whose expressions were wrong. The counts stay
// beside the tier, since `evaluations` is what tells the two apart.
TEST(ObservationEngineTest, NeverEvaluatedCaptureIsNotCalledUnavailable) {
  CaptureReport Capture;
  Capture.Expr = "I.Ty.TypeID";
  Capture.Evaluations = 0;

  // The default tier renders "unavailable", which is the word a capture that was
  // read and failed gets. A capture that was never read must not borrow it.
  std::string S = Render(Capture.Render());
  EXPECT_EQ(S, R"("not_evaluated")") << S;
  EXPECT_EQ(S.find("unavailable"), std::string::npos) << S;
}

// `$return` is read from the ABI's result location rather than resolved from a
// name, so it never evaluates. Falling through to the evaluation-count report
// would mark the one capture at a return site that cannot fail as unavailable.
TEST(ObservationEngineTest, ReturnValueCaptureReportsThatItCameFromTheABI) {
  CaptureReport Capture;
  Capture.Expr = "$return";
  Capture.FromABI = true;

  // A return value read at every hit of the return site, said as a count. It is
  // the only thing that can say so: `$return` is left out of `capture_failures`,
  // there being no name to correct, so before this the row was the word "abi"
  // whether the value had been readable at every hit or at none.
  Capture.Evaluations = 120;
  EXPECT_EQ(Render(Capture.Render()), "\"abi x120\"");

  // Never readable -- a function returning void is the everyday case, and it
  // looked perfectly healthy.
  Capture.Errors = 120;
  const std::string Failed = Render(Capture.Render());
  EXPECT_NE(Failed.find("\"errors\":120"), std::string::npos) << Failed;
  EXPECT_NE(Failed.find("\"tier\":\"abi\""), std::string::npos) << Failed;

  // And an observation that never fired is not a return value that failed. The
  // count is taken at every recorded hit rather than only where a value could be
  // read, which is what makes zero mean this and nothing else.
  Capture.Evaluations = 0;
  Capture.Errors = 0;
  EXPECT_EQ(Render(Capture.Render()), "\"not_evaluated\"");
}

// A capture the program recorded for itself resolved no name and ran no
// expression, so it has no tier and no cost -- and being told so is how a reader
// learns that the hit it came from cost no stop at all. `eval` on the observation
// only says the tracepoint's work is in the program; a condition compiled in
// still stops at every hit it lets through.
TEST(ObservationEngineTest, RecordedCaptureSaysTheProgramReadItAndNotAStop) {
  CaptureReport Capture;
  Capture.Expr = "bucket";
  Capture.InProcess = true;
  Capture.Evaluations = 20000;
  EXPECT_EQ(Render(Capture.Render()), "\"in_process x20000\"");

  // The tier would otherwise be the default, which renders "unavailable" -- the
  // word a capture that was read and failed gets.
  EXPECT_EQ(Render(Capture.Render()).find("unavailable"), std::string::npos);

  // A value the ring overwrote before it could be read is a value that did not
  // arrive, and the count is what says how many did.
  Capture.Errors = 12;
  const std::string Lost = Render(Capture.Render());
  EXPECT_NE(Lost.find("\"errors\":12"), std::string::npos) << Lost;
  EXPECT_NE(Lost.find("\"tier\":\"in_process\""), std::string::npos) << Lost;

  // And a tracepoint that never fired is not a value that failed to arrive.
  Capture.Evaluations = 0;
  Capture.Errors = 0;
  EXPECT_EQ(Render(Capture.Render()), "\"not_evaluated\"");
}

// `eval` is the answer to "which mode did this observation get", and an
// observation that reads values has that question whether or not it also tests a
// condition. Under the condition's own test it was reported for one and not the
// other, so a plan whose captures were being read at a stop per hit said nothing
// about it at all.
TEST(ObservationEngineTest, AnObservationThatOnlyReadsValuesStillSaysItsMode) {
  ObservationReport Report;
  Report.Label = "hot_step";
  Report.At = "hot_step";
  Report.HasCondition = false;
  Report.InProcess = true;
  CaptureReport Capture;
  Capture.Expr = "bucket";
  Capture.InProcess = true;
  Capture.Evaluations = 20000;
  Report.Captures.push_back(Capture);

  std::string S = Render(Report.Render());
  EXPECT_NE(S.find("\"eval\":\"in-process\""), std::string::npos) << S;
  // And no condition counters beside it, there being no condition.
  EXPECT_EQ(S.find("condition_true"), std::string::npos) << S;
}

// A bare hit counter has nothing for the program to do for itself, so there is
// nothing to report a mode about. Reported anyway, its absence would be the only
// way to tell "nothing was asked" from "the question was never answered".
TEST(ObservationEngineTest, ABareHitCounterHasNoModeToReport) {
  ObservationReport Report;
  Report.Label = "reached";
  Report.At = "compute_value";
  Report.Hits = 1;

  std::string S = Render(Report.Render());
  EXPECT_EQ(S.find("eval"), std::string::npos) << S;
}

TEST(ObservationEngineTest, DisabledCaptureCarriesItsNumbers) {
  CaptureReport Capture;
  Capture.Expr = "I->getName()";
  Capture.Tier = ValueResolutionTier::Expression;
  Capture.Evaluations = 10;
  Capture.TotalMs = 900.0;
  Capture.Disabled = AssessCaptureCost(Cost(Ms(900), 10, Ms(1000)));

  std::string S = Render(Capture.Render());
  EXPECT_NE(S.find("\"observed_hits\":10"), std::string::npos);
  EXPECT_NE(S.find("\"per_hit_ms\":90"), std::string::npos);
  // The fix is named once per observation and not once per capture, so it is not
  // here. What the capture itself has to say is what it cost.
  EXPECT_EQ(S.find("note"), std::string::npos) << S;
}

TEST(ObservationEngineTest, OneReasonCoveringSeveralCapturesIsStatedOnce) {
  // Measured on an observation taken at a function's return, where none of the
  // eight captures naming its locals could be read: the same sentence eight
  // times, 2.6 kB, differing only in the expression each quoted -- which is the
  // key it was already filed under.
  ObservationReport Report;
  Report.At = "guard";
  for (llvm::StringRef Expr : {"Low16", "Hi16", "OpIs16Bit"}) {
    CaptureReport Capture;
    Capture.Expr = Expr.str();
    Capture.Tier = ValueResolutionTier::Unresolved;
    Capture.Evaluations = 3;
    Capture.Errors = 3;
    Capture.Disabled = AssessCaptureCost(Cost(Ms(30), 3, Ms(1000), 3, true));
    Report.Captures.push_back(std::move(Capture));
  }

  const llvm::json::Value Rendered = Report.Render();
  const llvm::json::Array *Stopped =
      Rendered.getAsObject()->getArray("stopped");
  ASSERT_NE(Stopped, nullptr) << Render(Rendered);
  ASSERT_EQ(Stopped->size(), 1u) << Render(Rendered);
  const llvm::json::Object *Entry = (*Stopped)[0].getAsObject();
  EXPECT_EQ(Render(llvm::json::Value(llvm::json::Array(*Entry->getArray(
                "captures")))),
            R"(["Low16","Hi16","OpIs16Bit"])");
  EXPECT_NE(Entry->getString("note")->find("on\": \"entry"),
            llvm::StringRef::npos);
}

TEST(ObservationEngineTest, CycleIsReportedWhenOneWasFound) {
  ObservationResult Result;
  Result.Result = Outcome::TimedOut;
  Result.Cycle = CycleReport{2, 40, {"parse", "emit"}};

  std::string S = Render(Result.Render());
  EXPECT_NE(S.find("\"period\":2"), std::string::npos);
  EXPECT_NE(S.find("\"repeats\":40"), std::string::npos);
}

TEST(ObservationEngineTest, AnAggregateOverAPrefixSaysSoBesideItself) {
  // Every field of an aggregate is shaped the same whether the run reached the
  // end of the program or a ceiling stopped it a third of the way through, so a
  // value the run never got to is reported exactly as a value the program never
  // held. Beside the aggregate rather than inside it, because it qualifies all
  // of it.
  ObservationResult Result;
  Result.Result = Outcome::TimedOut;
  Result.Aggregate = llvm::json::Object{{"scale", llvm::json::Object{}}};
  Result.AggregateCoversPrefix = true;

  EXPECT_NE(Render(Result.Render()).find("\"aggregate_covers\":\"partial\""),
            std::string::npos);
}

TEST(ObservationEngineTest, AnAggregateOverAWholeRunIsNotQualified) {
  // Absent is what a reader takes as "this describes the program", which is the
  // ordinary case and must not spend a field saying so.
  ObservationResult Result;
  Result.Result = Outcome::Exited;
  Result.Aggregate = llvm::json::Object{{"scale", llvm::json::Object{}}};

  EXPECT_EQ(Render(Result.Render()).find("aggregate_covers"),
            std::string::npos);

  // Nor a crashed one, which is decided by the outcome and not by the field. A
  // crash is the program reaching its own end: every hit it made is in the
  // aggregate, no ceiling would have found more, and `outcome` says what happened
  // in the first field of the response. Treated as merely abnormal, the crashed
  // side of a comparison was told in 400 characters that its ceiling had ended it,
  // having run for a fifth of a second against a ninety-second one.
  EXPECT_TRUE(IsCeilingStop(Outcome::TimedOut));
  EXPECT_TRUE(IsCeilingStop(Outcome::NoProgress));
  EXPECT_FALSE(IsCeilingStop(Outcome::Crashed));
  EXPECT_FALSE(IsCeilingStop(Outcome::Exited));
}

TEST(ObservationEngineTest, TailIsInlinedOnlyWhenTheProgramEndedBadly) {
  EXPECT_FALSE(IsAbnormal(Outcome::Exited));
  EXPECT_TRUE(IsAbnormal(Outcome::Crashed));
  EXPECT_TRUE(IsAbnormal(Outcome::TimedOut));
  EXPECT_TRUE(IsAbnormal(Outcome::NoProgress));
}

TEST(ObservationEngineTest, ACeilingStopSaysWhatTheCeilingCounted) {
  // A caller reading `timeout_seconds: 40` beside `elapsed_ms: 45799` has to
  // account for 5.8 seconds it did not ask for, and the field that would -- the
  // setup time -- is suppressed on a long run for being a small share of it. Both
  // terms have to be in the one sentence that is always there.
  const std::string S = DescribeWallClock(/*RunningMs=*/40122.0,
                                          /*SetupMs=*/5677.0);
  EXPECT_NE(S.find("40.1s"), std::string::npos) << S;
  EXPECT_NE(S.find("5.7s"), std::string::npos) << S;
  // Which of the two the ceiling bounds, rather than leaving a reader to guess
  // from two numbers that both look like candidates.
  EXPECT_NE(S.find("ceiling"), std::string::npos) << S;
}

//===----------------------------------------------------------------------===//
// Stack profile
//===----------------------------------------------------------------------===//

namespace {

/// A stack, innermost first, from names paired with the file each resolved to.
/// A frame with no file is one whose definition the caller cannot see, which is
/// what separates a thread doing work from a thread waiting in a library.
std::vector<RawFrame> Stack(
    std::initializer_list<std::pair<llvm::StringRef, llvm::StringRef>> Frames) {
  std::vector<RawFrame> Out;
  for (const auto &[Function, File] : Frames) {
    RawFrame Frame;
    Frame.Function = Function.str();
    Frame.File = File.str();
    Frame.Line = File.empty() ? 0 : 10;
    Out.push_back(std::move(Frame));
  }
  return Out;
}

} // namespace

TEST(StackProfileTest, NothingSampledRendersNothing) {
  // A program that finishes before the first sample is due is the ordinary case,
  // and it must not grow an empty section for the sake of a uniform shape.
  StackProfile Profile;
  EXPECT_EQ(Profile.Samples(), 0u);
  EXPECT_TRUE(Profile.Render().getAsNull().has_value());
}

TEST(StackProfileTest, InnermostFrameIsWhatIsCounted) {
  StackProfile Profile;
  for (int I = 0; I < 7; ++I)
    Profile.Record(1, Stack({{"mix", "p.cpp"}, {"loop", "p.cpp"}}));
  for (int I = 0; I < 2; ++I)
    Profile.Record(1, Stack({{"loop", "p.cpp"}}));

  const std::string S = Render(Profile.Render());
  EXPECT_NE(S.find(R"("samples":9)"), std::string::npos) << S;
  // Self time: `loop` was on the stack for every sample and is where the program
  // was for two of them.
  EXPECT_NE(S.find(R"({"file":"p.cpp","function":"mix","line":10,"samples":7})"),
            std::string::npos)
      << S;
}

TEST(StackProfileTest, AWaitingThreadDoesNotOutrankAWorkingOne) {
  // Measured on a program with one thread spinning and one asleep: the sleeping
  // thread is sampled as often, and its innermost frame is the same every time
  // while the working thread's moves, so counting alone reported the idle thread
  // as the hottest place in the program.
  //
  // The counts are equal here because on a real program they are: every live
  // thread is recorded at every sample, so two threads alive across the same
  // samples tie exactly -- 14 against 14, 15 against 15 on the subjects this was
  // measured on. A fixture where the working thread has more samples tests a case
  // that does not arise.
  StackProfile Profile;
  for (int I = 0; I < 15; ++I)
    Profile.Record(2, Stack({{"__semwait_signal", ""}, {"parked", "p.cpp"}}));
  for (int I = 0; I < 15; ++I)
    Profile.Record(1, Stack({{"mix", "p.cpp"}, {"loop", "p.cpp"}}));

  const llvm::json::Value Rendered = Profile.Render();
  const llvm::json::Array *Hot = Rendered.getAsObject()->getArray("hot");
  ASSERT_NE(Hot, nullptr);
  ASSERT_EQ(Hot->size(), 2u);
  EXPECT_EQ((*Hot)[0].getAsObject()->getString("function"),
            std::optional<llvm::StringRef>("mix"));
  // Withheld from the front, not dropped: a thread waiting on something that
  // never arrives is the answer often enough to keep.
  EXPECT_EQ((*Hot)[1].getAsObject()->getString("function"),
            std::optional<llvm::StringRef>("__semwait_signal"));
  EXPECT_EQ(Rendered.getAsObject()->getInteger("threads"),
            std::optional<int64_t>(2));

  // `under` is the field that answers "what is this program doing", and it was
  // the one still deciding on sample counts alone: with the counts tied it went to
  // the lowest thread id, describing the sleeping thread while `hot` right above
  // it described the working one.
  EXPECT_EQ(Profile.BusiestThread(), 1u);
  const llvm::json::Array *Under = Rendered.getAsObject()->getArray("under");
  ASSERT_NE(Under, nullptr);
  EXPECT_EQ(Render(llvm::json::Value(llvm::json::Array(*Under))),
            R"(["mix","loop"])");
  // Which thread the path is of. `hot` says so per entry and this said nothing,
  // so a response could describe two threads without ever admitting it.
  EXPECT_EQ(Rendered.getAsObject()->getInteger("under_tid"),
            std::optional<int64_t>(1));
}

TEST(StackProfileTest, TwoEquallyBusyThreadsGetTheSameAnswerEveryRun) {
  // Two threads spinning identically tie on everything there is to rank them by,
  // and the answer then has to depend on nothing but their identity. Measured on a
  // pair of programs differing only in the order of two `pthread_create` calls,
  // `under` described alpha's stack in one and beta's in the other, which is a
  // report following creation order and saying nothing about the run.
  auto Sample = [](StackProfile &P, bool AlphaFirst) {
    for (int I = 0; I < 12; ++I) {
      if (AlphaFirst) {
        P.Record(10, Stack({{"alpha_grind", "p.c"}, {"alpha", "p.c"}}));
        P.Record(11, Stack({{"beta_grind", "p.c"}, {"beta", "p.c"}}));
      } else {
        P.Record(11, Stack({{"beta_grind", "p.c"}, {"beta", "p.c"}}));
        P.Record(10, Stack({{"alpha_grind", "p.c"}, {"alpha", "p.c"}}));
      }
    }
  };

  StackProfile One;
  Sample(One, /*AlphaFirst=*/true);
  StackProfile Other;
  Sample(Other, /*AlphaFirst=*/false);
  EXPECT_EQ(One.BusiestThread(), Other.BusiestThread());
  EXPECT_EQ(One.BusiestThread(), 10u);
}

TEST(StackProfileTest, ThreadRankingCountsWhatASampleHeldNotHowManyThereWere) {
  // A thread that ran for the whole run and a thread that was created for it are
  // recorded at the same samples, so the count separates neither from the other.
  // What separates them is whether their samples resolved to the caller's own
  // source, and how many distinct places they were found in.
  StackProfile Parked;
  for (int I = 0; I < 20; ++I) {
    Parked.Record(1, Stack({{"__ulock_wait", ""}, {"_pthread_join", ""}}));
    Parked.Record(2, Stack({{"advance", "p.c"}, {"worker", "p.c"}}));
  }
  EXPECT_EQ(Parked.BusiestThread(), 2u);

  // Neither thread resolved anything, so the tie falls to how many places each was
  // found in: a thread parked in a wait has exactly one for the whole run.
  StackProfile Blind;
  for (int I = 0; I < 20; ++I) {
    Blind.Record(1, Stack({{"__psynch_cvwait", ""}}));
    Blind.Record(2, Stack({{I % 2 ? "memcpy" : "memmove", ""}}));
  }
  EXPECT_EQ(Blind.BusiestThread(), 2u);

  // Nothing sampled has no busiest thread, which is what lets a caller fall back
  // to whatever the debugger selected.
  EXPECT_EQ(StackProfile().BusiestThread(), 0u);
}

TEST(StackProfileTest, TheCoveringPathIsWhereTheRunIsRatherThanWhereTheLeafIs) {
  // Measured on a compiler looping inside one analysis: the eight hottest sites
  // were one-line accessors with one or two samples each, and the function
  // actually spinning was in every sample and the leaf of none. Self time answers
  // the wrong question for an unoptimized build; what covers the samples answers
  // the right one.
  StackProfile Profile;
  const llvm::StringRef Leaves[] = {"isPresent", "capacity", "getValueID"};
  for (int I = 0; I < 9; ++I)
    Profile.Record(1, Stack({{Leaves[I % 3], "Casting.h"},
                             {"digRecurrence", "p.cpp"},
                             {"getRecurrences", "p.cpp"},
                             {"main", "p.cpp"}}));

  const llvm::json::Value Rendered = Profile.Render();
  const llvm::json::Array *Under = Rendered.getAsObject()->getArray("under");
  ASSERT_NE(Under, nullptr);
  // Innermost first, and the accessors are not on it: none of them covers half
  // the run.
  EXPECT_EQ(Render(llvm::json::Value(llvm::json::Array(*Under))),
            R"(["digRecurrence","getRecurrences","main"])");
}

TEST(StackProfileTest, OneStraySampleDoesNotEmptyTheCoveringPath) {
  // A run whose thread was in the dynamic loader for one sample out of ten shares
  // nothing across all of them, so a path defined as what every sample holds
  // collapses to nothing. A share of them is what it takes.
  StackProfile Profile;
  Profile.Record(1, Stack({{"dyld_start", ""}}));
  for (int I = 0; I < 9; ++I)
    Profile.Record(1, Stack({{"mix", "p.cpp"},
                             {"loop", "p.cpp"},
                             {"middle", "p.cpp"},
                             {"main", "p.cpp"}}));

  const llvm::json::Value Rendered = Profile.Render();
  const llvm::json::Array *Under = Rendered.getAsObject()->getArray("under");
  ASSERT_NE(Under, nullptr);
  EXPECT_EQ(Render(llvm::json::Value(llvm::json::Array(*Under))),
            R"(["mix","loop","middle","main"])");
}

TEST(StackProfileTest, ARecursiveFunctionCountsOncePerSample) {
  // Otherwise a function twenty frames deep in its own recursion covers a run of
  // one sample twenty times over, and every threshold on coverage is meaningless.
  StackProfile Profile;
  for (int I = 0; I < 3; ++I)
    Profile.Record(1, Stack({{"recurse", "p.cpp"},
                             {"recurse", "p.cpp"},
                             {"recurse", "p.cpp"},
                             {"main", "p.cpp"}}));
  for (int I = 0; I < 5; ++I)
    Profile.Record(1, Stack({{"other", "p.cpp"}, {"main", "p.cpp"}}));

  const llvm::json::Value Rendered = Profile.Render();
  const llvm::json::Array *Under = Rendered.getAsObject()->getArray("under");
  ASSERT_NE(Under, nullptr);
  // `recurse` covers three samples of eight and is left off; counted per frame it
  // would have covered nine of eight, and led the path.
  EXPECT_EQ(Render(llvm::json::Value(llvm::json::Array(*Under))),
            R"(["other","main"])");
}

TEST(StackProfileTest, HotIsBoundedAndSaysHowManyWereDropped) {
  StackProfile Profile;
  // One place holding a share of the run, so the list is worth enumerating at all;
  // without that the bound below is not what shortens it.
  for (size_t I = 0; I < 40; ++I)
    Profile.Record(1, Stack({{"hot", "p.cpp"}}));
  for (size_t I = 0; I < StackProfile::MaxHot + 5; ++I)
    Profile.Record(1, Stack({{"f" + std::to_string(I), "p.cpp"}}));

  const llvm::json::Value Rendered = Profile.Render();
  const llvm::json::Object *O = Rendered.getAsObject();
  ASSERT_NE(O, nullptr);
  EXPECT_EQ(O->getArray("hot")->size(), StackProfile::MaxHot);
  EXPECT_EQ(O->getInteger("hot_elided"), std::optional<int64_t>(6));
}

TEST(StackProfileTest, ASingleThreadIsNotWorthNaming) {
  // The thread id is what tells two threads apart, and there is nothing to tell
  // apart in a program that has one.
  StackProfile Profile;
  Profile.Record(7, Stack({{"mix", "p.cpp"}}));

  const std::string S = Render(Profile.Render());
  EXPECT_EQ(S.find("tid"), std::string::npos) << S;
  EXPECT_EQ(S.find("threads"), std::string::npos) << S;
}

TEST(CollapseTemplateArgumentsTest, ArgumentsGoAndTheNameStays) {
  // Measured on one frame of a compiler's instruction selector: 323 characters,
  // of which 250 were two spellings of an intrusive list iterator.
  EXPECT_EQ(CollapseTemplateArguments(
                "llvm::SelectionDAGISel::SelectBasicBlock(llvm::ilist_iterator_"
                "w_bits<llvm::ilist_detail::node_options<llvm::Instruction, "
                "true, false, void, true, llvm::BasicBlock>, false, true>, "
                "bool&)"),
            "llvm::SelectionDAGISel::SelectBasicBlock(llvm::ilist_iterator_w_"
            "bits<...>, bool&)");
}

TEST(CollapseTemplateArgumentsTest, NestedListsCollapseWithTheirEnclosingOne) {
  EXPECT_EQ(CollapseTemplateArguments("f(std::map<int, std::vector<char>>&)"),
            "f(std::map<...>&)");
}

TEST(CollapseTemplateArgumentsTest, AnOperatorKeepsItsAngleBrackets) {
  // `operator<` and `operator<<` are names. A collapser that took every `<` as
  // opening an argument list would eat the rest of the frame.
  EXPECT_EQ(CollapseTemplateArguments("llvm::operator<<(llvm::raw_ostream&)"),
            "llvm::operator<<(llvm::raw_ostream&)");
  EXPECT_EQ(CollapseTemplateArguments("Foo::operator<(Foo const&) const"),
            "Foo::operator<(Foo const&) const");
}

TEST(CollapseTemplateArgumentsTest, ANameWithNoTemplatesIsUnchanged) {
  EXPECT_EQ(CollapseTemplateArguments("matchPERM(llvm::SDNode*)"),
            "matchPERM(llvm::SDNode*)");
  EXPECT_EQ(CollapseTemplateArguments(""), "");
}

TEST(CollapseTemplateArgumentsTest, AnUnclosedListDoesNotRunAway) {
  // A truncated name is not a reason to lose the part that arrived.
  EXPECT_EQ(CollapseTemplateArguments("f<int"), "f<...>");
}

TEST(StackProfileTest, SamplesOnlyOutsideTheProgramAreNotAProfile) {
  // A run that crashed a fifth of a second after launch was sampled once, while
  // the dynamic loader was still mapping images. Reporting that as where the
  // program spent its time is worse than reporting nothing.
  StackProfile Profile;
  Profile.Record(1, Stack({{"dyld4::prepare", ""}, {"dyld_start", ""}}));
  EXPECT_EQ(Profile.Samples(), 1u);
  EXPECT_TRUE(Profile.Render().getAsNull().has_value());
}

TEST(StackProfileTest, WhereNoPlaceDominatesOneStandsForTheList) {
  // Measured on a compiler looping inside one analysis: nineteen samples spread
  // over seventeen accessors, none holding more than two. Naming eight of them
  // and eliding nine describes the last instruction of an inlined getter, while
  // the covering path names the function responsible.
  StackProfile Profile;
  for (int I = 0; I < 17; ++I)
    Profile.Record(1, Stack({{"accessor" + std::to_string(I), "Casting.h"},
                             {"foldShuffleToIdentity", "p.cpp"}}));

  const llvm::json::Value Rendered = Profile.Render();
  const llvm::json::Object *O = Rendered.getAsObject();
  ASSERT_NE(O, nullptr);
  EXPECT_EQ(O->getArray("hot")->size(), 1u);
  EXPECT_EQ(O->getInteger("hot_elided"), std::optional<int64_t>(16));
  // The place is still named concretely enough to open a file at.
  EXPECT_EQ((*O->getArray("hot"))[0].getAsObject()->getString("file"),
            std::optional<llvm::StringRef>("Casting.h"));
  EXPECT_EQ(Render(llvm::json::Value(
                llvm::json::Array(*O->getArray("under")))),
            R"(["foldShuffleToIdentity"])");
}

TEST(StackProfileTest, ARankingOfOnesAndTwosIsNotARankingHoweverLargeItsShare) {
  // A share alone has no floor, and the runs it has to hold for are small: 7
  // samples over 25 s of pinned CPU, then 12, 16, 17, 19, 31, with top entries of
  // one or two samples apiece. Two samples of sixteen is an eighth exactly, which
  // the share test passes -- so the collapse fired at seventeen samples and not at
  // sixteen, and in between printed a ranking of two, two, one, one, one, one.
  StackProfile Profile;
  for (int I = 0; I < 8; ++I)
    Profile.Record(1, Stack({{"leaf" + std::to_string(I % 6), "Casting.h"},
                             {"spin", "p.cpp"}}));

  const llvm::json::Value Rendered = Profile.Render();
  const llvm::json::Object *Flat = Rendered.getAsObject();
  ASSERT_NE(Flat, nullptr);
  ASSERT_EQ(Profile.Samples(), 8u);
  EXPECT_EQ(Flat->getArray("hot")->size(), 1u);
  EXPECT_EQ(Flat->getInteger("hot_elided"), std::optional<int64_t>(5));
}

TEST(StackProfileTest, FourSamplesInOnePlaceIsEnoughToRankBy) {
  // The boundary belongs to the ranking: at four samples a count is evidence about
  // a place, which is the bar a whole run has to clear before its samples say
  // anything at all.
  StackProfile Profile;
  for (int I = 0; I < 4; ++I)
    Profile.Record(1, Stack({{"hot", "p.cpp"}, {"spin", "p.cpp"}}));
  for (int I = 0; I < 2; ++I)
    Profile.Record(1, Stack({{"warm", "p.cpp"}, {"spin", "p.cpp"}}));

  const llvm::json::Value Rendered = Profile.Render();
  const llvm::json::Object *Ranked = Rendered.getAsObject();
  ASSERT_NE(Ranked, nullptr);
  EXPECT_EQ(Ranked->getArray("hot")->size(), 2u);
  EXPECT_EQ(Ranked->getInteger("hot_elided"), std::nullopt);
}

TEST(ObservationEngineTest, ACaptureOnAnObservationThatNeverFiredIsAWord) {
  // A tracepoint that was never hit has captures with nothing to report, and their
  // numbers are all zero. Measured on a plan whose three observations included two
  // that never fired: six fields saying what two words say.
  CaptureReport Capture;
  Capture.Expr = "ALoad->dump()";
  EXPECT_EQ(Render(Capture.Render()), R"("not_evaluated")");

  // Still distinct from a capture that was evaluated and could not be read, which
  // is the distinction the rest of the report is built to keep.
  Capture.Evaluations = 3;
  Capture.Errors = 3;
  Capture.Tier = ValueResolutionTier::Unresolved;
  EXPECT_NE(Render(Capture.Render()).find("unavailable"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Capture failure classification
//===----------------------------------------------------------------------===//

TEST(ObservationEngineTest, AMemberATypeDoesNotHaveIsAStableFailure) {
  // The base of the path resolved, or the compiler would not have got as far as
  // objecting to the member. A type's members come from its own definition, so
  // no later hit and no library loading can add the one that is missing.
  EXPECT_EQ(ClassifyCaptureFailure(
                "error: no member named 'chidren' in 'llvm::SDNode'"),
            CaptureFailure::UnknownMember);
  EXPECT_TRUE(IsStableFailure(CaptureFailure::UnknownMember));
}

TEST(ObservationEngineTest, ANameThatIsNotInScopeIsAStableFailure) {
  EXPECT_EQ(ClassifyCaptureFailure("error: use of undeclared identifier 'Idx'"),
            CaptureFailure::UnknownName);
  EXPECT_TRUE(IsStableFailure(CaptureFailure::UnknownName));
}

TEST(ObservationEngineTest, AFixitThatDidNotRepairTheShapeIsAStableFailure) {
  // A pointer/dot confusion carries a fixit, and the evaluator applies it and
  // retries by itself. Reaching a failure with this wording means the retry
  // failed too, so the expression is wrong about the types rather than merely
  // misspelled.
  EXPECT_EQ(ClassifyCaptureFailure("error: member reference type 'SDNode *' is "
                                   "a pointer; did you mean to use '->'?"),
            CaptureFailure::Malformed);
  EXPECT_EQ(ClassifyCaptureFailure(
                "error: member reference base type 'int' is not a structure or "
                "union"),
            CaptureFailure::Malformed);
  EXPECT_TRUE(IsStableFailure(CaptureFailure::Malformed));
}

TEST(ObservationEngineTest, AValueMerelyAbsentHereIsNotAStableFailure) {
  // Optimised out at this location, or reached through a pointer that is null
  // just now: the next hit is a fresh question and the capture is kept.
  EXPECT_EQ(ClassifyCaptureFailure("variable not available"),
            CaptureFailure::Situational);
  EXPECT_EQ(ClassifyCaptureFailure("parent is NULL"),
            CaptureFailure::Situational);
  EXPECT_FALSE(IsStableFailure(CaptureFailure::Situational));
}

TEST(ObservationEngineTest, AnUnrecognisedDiagnosticIsTreatedAsSituational) {
  // The classification reads compiler wording, which changes. Failing that way
  // has to cost a capture some retries rather than abandon one that would have
  // worked, so anything unknown is the class that is never given up on.
  EXPECT_EQ(ClassifyCaptureFailure("error: something nobody has seen yet"),
            CaptureFailure::Situational);
  EXPECT_EQ(ClassifyCaptureFailure(""), CaptureFailure::Situational);
}

TEST(ObservationEngineTest, AStableFailureIsStoppedAtTheFirstHit) {
  // Well inside the time budget, so this cannot be the cost test firing.
  CaptureCostInput In = Cost(Ms(1), 1, Ms(100000));
  In.Tier = ValueResolutionTier::Unresolved;
  In.Errors = 1;
  In.Failure = CaptureFailure::UnknownMember;

  CaptureCostDecision Off = AssessCaptureCost(In);
  ASSERT_TRUE(Off.Disable);
  // The reason and the candidate names are real JSON at top level, so the note
  // points there rather than saying it again at length in prose.
  EXPECT_NE(Off.Note.find("capture_failures"), std::string::npos) << Off.Note;
}

TEST(ObservationEngineTest, ASituationalFailureStillGetsItsAttempts) {
  // The three attempts exist for a pointer that is null at the first hit and
  // set at the second, and classifying a failure does not take them away.
  CaptureCostInput In = Cost(Ms(1), 1, Ms(100000));
  In.Tier = ValueResolutionTier::Unresolved;
  In.Errors = 1;
  In.Failure = CaptureFailure::Situational;
  EXPECT_FALSE(AssessCaptureCost(In).Disable);

  In.ObservedHits = UnresolvableCaptureAttempts;
  In.TotalHits = UnresolvableCaptureAttempts;
  In.Errors = UnresolvableCaptureAttempts;
  EXPECT_TRUE(AssessCaptureCost(In).Disable);
}

TEST(ObservationEngineTest, AReturnSiteKeepsItsOwnExplanation) {
  // A capture of a function's own local, taken at its return, fails stably --
  // and "no such name" is the wrong thing to tell that caller, whose
  // declaration is in exactly the right place. The more specific cause wins.
  CaptureCostInput In = Cost(Ms(1), 1, Ms(100000), /*Errors=*/1,
                             /*AtReturn=*/true);
  In.Failure = CaptureFailure::UnknownName;

  CaptureCostDecision Off = AssessCaptureCost(In);
  ASSERT_TRUE(Off.Disable);
  EXPECT_NE(Off.Note.find("already been popped"), std::string::npos)
      << Off.Note;
}

TEST(ObservationEngineTest, TheMemberNameWinsWhenAnExpressionDrawsBoth) {
  // Measured on `o.chidren` where `o` is an `Outer *`: the evaluator objects
  // both that the dot should be an arrow and that the member does not exist,
  // and emits them in that order. Applying the arrow reaches the same type and
  // still finds no such member, so the member name is the fault -- and it is
  // the reading with somewhere useful to look for candidates.
  const llvm::StringRef Both =
      "error: <user expression 0>:1:2: member reference type 'Outer *' is a "
      "pointer; did you mean to use '->'?\n"
      "error: <user expression 0>:1:3: no member named 'chidren' in 'Outer'";
  EXPECT_EQ(ClassifyCaptureFailure(Both), CaptureFailure::UnknownMember);

  // And the detail reported beside that reason is about the same thing. Taking
  // the first error line would pair "no_such_member" with a sentence about
  // arrows, which reads as the classification having gone wrong.
  const std::string Described =
      DescribeCaptureFailure(Both, CaptureFailure::UnknownMember, 400);
  EXPECT_NE(Described.find("no member named"), std::string::npos) << Described;
  EXPECT_EQ(Described.find("did you mean"), std::string::npos) << Described;
}

TEST(ObservationEngineTest, ADescriptionWithNoDecidingLineFallsBackToTheWhole) {
  // An execution failure is one line that is itself the diagnostic, with no
  // marker for any class to match, and it still has to be reported.
  const std::string Described = DescribeCaptureFailure(
      "Couldn't apply expression side effects : couldn't read its memory",
      CaptureFailure::Situational, 400);
  EXPECT_NE(Described.find("side effects"), std::string::npos) << Described;
}

//===----------------------------------------------------------------------===//
// Capture candidates
//===----------------------------------------------------------------------===//

TEST(ObservationEngineTest, CandidatesPutAPlausibleMisspellingFirst) {
  CaptureCandidates Found = RankCaptureCandidates(
      "Node.chidren", {"parent", "children", "name", "flags"});
  ASSERT_FALSE(Found.Names.empty());
  EXPECT_EQ(Found.Names.front(), "children");
  EXPECT_EQ(Found.InScope, 4u);
}

TEST(ObservationEngineTest, CandidatesCompareOnTheLastComponentAlone) {
  // Compared whole, every field of the type is equally far from the expression
  // and the ranking says nothing. `->` and `[` divide a path as `.` does.
  EXPECT_EQ(
      RankCaptureCandidates("I->Ty->TypeI", {"TypeID", "Bits"}).Names.front(),
      "TypeID");
  EXPECT_EQ(RankCaptureCandidates("Ops[0].Vals", {"Val", "User"}).Names.front(),
            "Val");
}

TEST(ObservationEngineTest, CandidatesFallBackToWhatWasActuallyInScope) {
  // A caller who misremembered a name rather than mistyping it gets no near
  // match, and a ranking that answered them with nothing would waste the one
  // piece of evidence there is.
  CaptureCandidates Found =
      RankCaptureCandidates("Count", {"Ty", "Ops", "Flags"});
  EXPECT_EQ(Found.Names, std::vector<std::string>({"Flags", "Ops", "Ty"}));
}

TEST(ObservationEngineTest, CandidatesAreBoundedAndSayWhatTheyAreDrawnFrom) {
  std::vector<std::string> Wide;
  for (unsigned I = 0; I != 40; ++I)
    Wide.push_back(("Field" + std::to_string(I)).c_str());

  CaptureCandidates Found = RankCaptureCandidates("Missing", Wide);
  EXPECT_EQ(Found.Names.size(), MaxCaptureCandidates);
  // Otherwise six names read as everything the type had.
  EXPECT_EQ(Found.InScope, 40u);
}

TEST(ObservationEngineTest, CandidatesFromNothingAreNothing) {
  CaptureCandidates Found = RankCaptureCandidates("Anything", {});
  EXPECT_TRUE(Found.Names.empty());
  EXPECT_EQ(Found.InScope, 0u);
}

//===----------------------------------------------------------------------===//
// Failure reporting
//===----------------------------------------------------------------------===//

TEST(ObservationEngineTest, AnAdoptedFixitIsReportedBesideTheCallersSpelling) {
  // The run is not evaluating what the caller wrote. Beside the key, which is
  // their own spelling, this is what connects the value to the expression.
  CaptureReport Capture;
  Capture.Expr = "I.Ty";
  Capture.FixedExpr = "I->Ty";
  Capture.Tier = ValueResolutionTier::Expression;
  Capture.Evaluations = 12;

  std::string S = Render(Capture.Render());
  EXPECT_NE(S.find("\"fixed_as\":\"I->Ty\""), std::string::npos) << S;
}

TEST(ObservationEngineTest, AFixitSurvivesTheShortcutsForAQuietCapture) {
  // A capture that resolved cheaply and never failed renders as one word. A
  // rewrite is not something a word can carry, so the shortcut must not swallow
  // it.
  CaptureReport Capture;
  Capture.Expr = "I.Ty";
  Capture.FixedExpr = "I->Ty";
  Capture.Tier = ValueResolutionTier::VariablePath;
  Capture.Evaluations = 3;
  EXPECT_NE(Render(Capture.Render()).find("fixed_as"), std::string::npos);

  // And a capture on an observation that never fired still renders as a word.
  CaptureReport Never;
  Never.Expr = "I.Ty";
  EXPECT_EQ(Render(Never.Render()), R"("not_evaluated")");
}

TEST(ObservationEngineTest, AStableFailureIsRealJSONAtTopLevel) {
  // The shape this replaces is a JSON document escaped into a string nested two
  // levels under a label, which is where a reader stops reading.
  ObservationResult Result;
  Result.Terminal.Description = "exited";

  CaptureFailureReport Failure;
  Failure.Label = "visit";
  Failure.Expr = "Node.chidren";
  Failure.Kind = CaptureFailure::UnknownMember;
  Failure.Reason = "no member named 'chidren' in 'llvm::SDNode'";
  Failure.Candidates.Names = {"children", "parent"};
  Failure.Candidates.InScope = 9;
  Failure.Disabled = true;
  Result.CaptureFailures.push_back(std::move(Failure));

  const llvm::json::Value Rendered = Result.Render();
  const llvm::json::Array *Failures =
      Rendered.getAsObject()->getArray("capture_failures");
  ASSERT_NE(Failures, nullptr);
  ASSERT_EQ(Failures->size(), 1u);

  const llvm::json::Object *Entry = (*Failures)[0].getAsObject();
  ASSERT_NE(Entry, nullptr);
  EXPECT_EQ(Entry->getString("observation"),
            std::optional<llvm::StringRef>("visit"));
  // Keyed on what the caller wrote, so this entry and the aggregate name the
  // same thing.
  EXPECT_EQ(Entry->getString("capture"),
            std::optional<llvm::StringRef>("Node.chidren"));
  EXPECT_EQ(Entry->getString("reason"),
            std::optional<llvm::StringRef>("no_such_member"));
  EXPECT_EQ(Entry->getBoolean("disabled"), std::optional<bool>(true));
  // Candidates are a JSON array of names, not a sentence to be parsed.
  ASSERT_NE(Entry->getArray("candidates"), nullptr);
  EXPECT_EQ(Render(llvm::json::Value(
                llvm::json::Array(*Entry->getArray("candidates")))),
            R"(["children","parent"])");
  EXPECT_EQ(Entry->getInteger("candidates_of"), std::optional<int64_t>(9));
}

TEST(ObservationEngineTest, ASuggestedFixitDoesNotReadLikeAnAppliedOne) {
  // A fixit the compiler could not make work either must not be adopted, and a
  // caller who read it as what ran would rewrite a capture to a spelling that
  // has been shown to fail.
  CaptureFailureReport Failure;
  Failure.Label = "visit";
  Failure.Expr = "I.Ty";
  Failure.FixedExpr = "I->Ty";
  Failure.FixApplied = false;

  std::string S = Render(Failure.Render());
  EXPECT_NE(S.find("\"fixed_as\":\"I->Ty\""), std::string::npos) << S;
  EXPECT_NE(S.find("\"fix_applied\":false"), std::string::npos) << S;
}

TEST(ObservationEngineTest, AConditionThatNeverEvaluatedSaysSoAsACondition) {
  // A condition that cannot be evaluated records no hits, which looks exactly
  // like an observation whose values were all uninteresting.
  CaptureFailureReport Failure;
  Failure.Label = "visit";
  Failure.Expr = "Node.knd == 3";
  Failure.IsCondition = true;

  EXPECT_NE(Render(Failure.Render()).find("\"field\":\"when\""),
            std::string::npos);
}

TEST(ObservationEngineTest, APassingPlanRendersNoFailureArrayAtAll) {
  // The array is additive. A plan whose captures all resolved has to render
  // exactly as it did before, or every consumer of the aggregate breaks.
  ObservationResult Result;
  Result.Terminal.Description = "exited";
  ObservationReport Report;
  Report.Label = "visit";
  Report.At = "visit";
  Report.ResolvedLocations = 1;
  Report.Hits = 12;
  CaptureReport Capture;
  Capture.Expr = "I.Ty";
  Capture.Tier = ValueResolutionTier::VariablePath;
  Capture.Evaluations = 12;
  Report.Captures.push_back(std::move(Capture));
  Result.Observations.push_back(std::move(Report));

  std::string S = Render(Result.Render());
  EXPECT_EQ(S.find("capture_failures"), std::string::npos) << S;
  // And the nested per-capture rendering is untouched.
  EXPECT_NE(S.find("\"captures\":{\"I.Ty\":\"path x12\"}"), std::string::npos)
      << S;
}

TEST(ObservationEngineTest, AnOutOfScopeNameKeepsItsAttemptsAcrossLocations) {
  // A name is looked up at a program counter, and an observation on a function
  // name resolves to one location per inlined copy -- each with its own
  // variables in scope. Dropping the capture at the first hit would lose it at
  // every later location that did hold it, and losing data is worse than two
  // more compiles.
  CaptureCostInput In = Cost(Ms(1), 1, Ms(100000));
  In.Tier = ValueResolutionTier::Unresolved;
  In.Errors = 1;
  In.Failure = CaptureFailure::UnknownName;
  In.Locations = 27;
  EXPECT_FALSE(AssessCaptureCost(In).Disable);

  // One location fixes the program counter, so there is no later scope for the
  // name to appear in and the first failure settles it.
  In.Locations = 1;
  EXPECT_TRUE(AssessCaptureCost(In).Disable);
}

TEST(ObservationEngineTest, AMissingMemberSettlesAtOneFailureAnywhere) {
  // A type's members are not a fact about a location, so however many locations
  // the tracepoint has, no later one has a different answer.
  CaptureCostInput In = Cost(Ms(1), 1, Ms(100000));
  In.Tier = ValueResolutionTier::Unresolved;
  In.Errors = 1;
  In.Failure = CaptureFailure::UnknownMember;
  In.Locations = 27;
  EXPECT_TRUE(AssessCaptureCost(In).Disable);
}

namespace {

/// A capture spelled as a call to a printer, which is the shape the whole of
/// \ref PrintedAsValue exists for.
InferiorOutput Wrote(llvm::StringRef Out, llvm::StringRef Err) {
  InferiorOutput O;
  O.Out = Out.str();
  O.Err = Err.str();
  return O;
}

} // namespace

TEST(PrintedValueTest, WhatAPrinterPrintedIsTheValue) {
  // Not `(void)`. A histogram over a printer has to count dumps, or the run can
  // say that a line executed and nothing about what the node there was.
  const PrintedValue V = PrintedAsValue(Wrote("", "t12: xor"), 300);
  EXPECT_EQ(V.Text, "t12: xor");
  EXPECT_FALSE(V.Shortened);
}

TEST(PrintedValueTest, ATrailingNewlineIsNotPartOfTheValue) {
  // Near-universal in dump output, and left in it makes every key differ from
  // its own trimmed form -- so the same node dumped by a printer that terminates
  // its line and one that does not would be two values.
  EXPECT_EQ(PrintedAsValue(Wrote("", "t12: xor\n"), 300).Text, "t12: xor");
  EXPECT_EQ(PrintedAsValue(Wrote("", "\n\t t12: xor \n\n"), 300).Text,
            "t12: xor");
}

TEST(PrintedValueTest, TheSameTextKeysTheSameThroughEitherStream) {
  // Standard output is on a terminal and standard error is a file of the run's
  // own, so one arrives with CRLF and the other with LF. Keyed as written, a
  // printer that chose `outs()` would be summarised separately from the same
  // printer through `errs()`.
  EXPECT_EQ(PrintedAsValue(Wrote("add\r\n  op 0: t1\r\n", ""), 300).Text,
            PrintedAsValue(Wrote("", "add\n  op 0: t1\n"), 300).Text);
}

TEST(PrintedValueTest, TheInteriorOfAMultiLineDumpIsKept) {
  // A dump of a node with operands is a subtree, and the subtree is the answer.
  // Folding it to one line would merge two nodes differing only in an operand.
  EXPECT_EQ(PrintedAsValue(Wrote("", "xor\n  op 0: t1\n  op 1: t2\n"), 300).Text,
            "xor\n  op 0: t1\n  op 1: t2");
}

TEST(PrintedValueTest, WhitespaceAloneIsNotAValue) {
  // What a drain that caught only the newline the program had left buffered
  // looks like. Filed as a value it becomes a histogram bucket no expression
  // produced.
  EXPECT_TRUE(PrintedAsValue(Wrote("", "\n"), 300).Text.empty());
  EXPECT_TRUE(PrintedAsValue(Wrote("", ""), 300).Text.empty());
}

TEST(PrintedValueTest, TwoStreamsAtOnceHaveNoOrderSoNeitherIsTheValue) {
  // One file and one pty, with nothing marking which write came first. A value
  // joining them would report a sequence the program did not have, so the hit
  // keeps the void marker and its `printed`.
  EXPECT_TRUE(PrintedAsValue(Wrote("on stdout", "on stderr"), 300).Text.empty());
}

TEST(PrintedValueTest, AKeyPastTheBoundSaysItIsAPrefixAndHowMuchWent) {
  // A `dump()` on a compiler node runs to hundreds of bytes, and a key appears
  // once per histogram entry, twice per transition and once per outlier. What is
  // never allowed is for the cut text to read as the whole of what was printed.
  const std::string Long(400, 'x');
  const PrintedValue V = PrintedAsValue(Wrote("", Long), 300);
  EXPECT_TRUE(V.Shortened);
  EXPECT_EQ(V.Text.compare(0, 300, Long, 0, 300), 0);
  EXPECT_NE(V.Text.find("(+100 more chars"), std::string::npos) << V.Text;
}

TEST(PrintedValueTest, TwoDumpsSharingAPrefixAndALengthAreTwoValues) {
  // The same node with one operand changed is exactly what a caller is looking
  // for, and it differs past the bound. Keyed on the prefix alone the two would
  // be counted as one value -- a histogram that merges two answers says
  // something false, where one that splits one answer only says less.
  std::string A(400, 'x');
  std::string B = A;
  B[380] = 'y';
  EXPECT_NE(PrintedAsValue(Wrote("", A), 300).Text,
            PrintedAsValue(Wrote("", B), 300).Text);
}

TEST(ObservationEngineTest, ACostNoteDoesNotClaimATotalItCannotKnow) {
  // The decision is taken while the program is still running, so the only total
  // available is the hits reached so far -- which made every such note read
  // "after 66 of 66 hits", a fraction of one, for a run that went on to have
  // four thousand. The share belongs to the rendered numbers, which are
  // corrected once the run's total is known.
  CaptureCostInput In = Cost(Ms(5000), 66, Ms(19639));
  In.Tier = ValueResolutionTier::Expression;
  In.Expr = "N->dump()";
  const CaptureCostDecision D = AssessCaptureCost(In);
  ASSERT_TRUE(D.Disable);
  EXPECT_NE(D.Note.find("after 66 hits"), std::string::npos) << D.Note;
  EXPECT_EQ(D.Note.find("66 of 66"), std::string::npos) << D.Note;
}

TEST(ObservationEngineTest, AStoppedCaptureSaysWhatItsAggregateCovers) {
  // A capture turned off partway leaves the aggregate a sample presented as a
  // summary. Measured on a printer stopped after 66 of 4000 hits: four buckets
  // totalling 66, with nothing saying the other 3934 were not in them. `hits`
  // and `observed_hits` sit in different objects, so noticing meant comparing
  // two numbers three fields apart.
  ObservationReport Report;
  Report.At = "visit";
  Report.Hits = 4000;

  CaptureReport Capture;
  Capture.Expr = "N->dump()";
  Capture.Tier = ValueResolutionTier::Expression;
  Capture.Evaluations = 66;
  CaptureCostDecision Stopped;
  Stopped.Disable = true;
  Stopped.ObservedHits = 66;
  Stopped.TotalHits = 4000;
  Stopped.Note = "stopped evaluating \"N->dump()\" after 66 hits.";
  Capture.Disabled = std::move(Stopped);
  Report.Captures.push_back(std::move(Capture));

  const std::string S = Render(Report.Render());
  EXPECT_NE(S.find("covers those 66 hits and not the 4000"), std::string::npos)
      << S;
}

TEST(ObservationEngineTest, ACaptureThatReadNothingHasNoPartialAggregateToWarnOf) {
  // A capture stopped because it never resolved contributed nothing to the
  // aggregate, so there is no sample to mistake for a summary, and its own
  // reason already says why it stopped. The coverage sentence there would be a
  // second explanation of the same fact.
  ObservationReport Report;
  Report.At = "visit";
  Report.Hits = 4000;

  CaptureReport Capture;
  Capture.Expr = "Node.chidren";
  Capture.Tier = ValueResolutionTier::Unresolved;
  Capture.Evaluations = 1;
  Capture.Errors = 1;
  CaptureCostDecision Stopped;
  Stopped.Disable = true;
  Stopped.ObservedHits = 1;
  Stopped.Note = "stopped at the first failure.";
  Capture.Disabled = std::move(Stopped);
  Report.Captures.push_back(std::move(Capture));

  EXPECT_EQ(Render(Report.Render()).find("covers those"), std::string::npos);
}

TEST(ObservationEngineTest, ALineWhoseSourceMovedSaysSoBesideResolvedLocations) {
  // `resolved_locations` is what a reader takes as the assurance that the
  // tracepoint is where they asked for, and it is true and says nothing about
  // whether the line still is what it was when the binary was built. The two
  // belong in the same object for that reason.
  ObservationReport Report;
  Report.At = "X86ISelLowering.cpp:49038";
  Report.ResolvedLocations = 1;
  Report.Hits = 12;
  Report.SourceNewerThanBinary = "\"X86ISelLowering.cpp\" was written 4m after "
                                 "the binary holding its line table";

  const std::string S = Render(Report.Render());
  EXPECT_NE(S.find("\"resolved_locations\":1"), std::string::npos) << S;
  EXPECT_NE(S.find("source_newer_than_binary"), std::string::npos) << S;
}

TEST(ObservationEngineTest, AnObservationWhoseSourceIsOlderCarriesNoSuchField) {
  // Additive, and silent by default: the field's presence is the whole signal,
  // so a healthy run must not carry it saying nothing is wrong.
  ObservationReport Report;
  Report.At = "visit";
  Report.ResolvedLocations = 1;
  EXPECT_EQ(Render(Report.Render()).find("source_newer"), std::string::npos);
}
