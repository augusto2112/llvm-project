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

CaptureCostInput Cost(Ms Spent, uint64_t ObservedHits, Ms Remaining) {
  CaptureCostInput In;
  In.Tier = ValueResolutionTier::Expression;
  In.Spent = Spent;
  In.ObservedHits = ObservedHits;
  In.TotalHits = ObservedHits;
  In.Remaining = Remaining;
  In.Expr = "I->getName()";
  return In;
}

RawFrame Frame(std::string Function, std::string File, uint32_t Line = 1) {
  return RawFrame{std::move(Function), std::move(File), Line};
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

TEST(ObservationEngineTest, PathCaptureCollapsesToItsTier) {
  CaptureReport Capture;
  Capture.Expr = "i";
  Capture.Tier = ValueResolutionTier::VariablePath;
  Capture.Evaluations = 4012;
  EXPECT_EQ(Render(Capture.Render()), "\"path\"");
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

  std::string S = Render(Capture.Render());
  EXPECT_NE(S.find("\"tier\":\"not_evaluated\""), std::string::npos) << S;
  EXPECT_NE(S.find("\"evaluations\":0"), std::string::npos) << S;
  EXPECT_EQ(S.find("unavailable"), std::string::npos) << S;
}

// `$return` is read from the ABI's result location rather than resolved from a
// name, so it never evaluates. Falling through to the evaluation-count report
// would mark the one capture at a return site that cannot fail as unavailable.
TEST(ObservationEngineTest, ReturnValueCaptureReportsThatItCameFromTheABI) {
  CaptureReport Capture;
  Capture.Expr = "$return";
  Capture.Evaluations = 0;
  Capture.FromABI = true;

  EXPECT_EQ(Render(Capture.Render()), "\"abi\"");
}

TEST(ObservationEngineTest, DisabledCaptureCarriesItsNumbersAndTheFix) {  CaptureReport Capture;
  Capture.Expr = "I->getName()";
  Capture.Tier = ValueResolutionTier::Expression;
  Capture.Evaluations = 10;
  Capture.TotalMs = 900.0;
  Capture.Disabled = AssessCaptureCost(Cost(Ms(900), 10, Ms(1000)));

  std::string S = Render(Capture.Render());
  EXPECT_NE(S.find("\"observed_hits\":10"), std::string::npos);
  EXPECT_NE(S.find("\"per_hit_ms\":90"), std::string::npos);
  EXPECT_NE(S.find("I->Name"), std::string::npos);
}

TEST(ObservationEngineTest, CycleIsReportedWhenOneWasFound) {
  ObservationResult Result;
  Result.Result = Outcome::TimedOut;
  Result.Cycle = CycleReport{2, 40, {"parse", "emit"}};

  std::string S = Render(Result.Render());
  EXPECT_NE(S.find("\"period\":2"), std::string::npos);
  EXPECT_NE(S.find("\"repeats\":40"), std::string::npos);
}

TEST(ObservationEngineTest, TailIsInlinedOnlyWhenTheProgramEndedBadly) {
  EXPECT_FALSE(IsAbnormal(Outcome::Exited));
  EXPECT_TRUE(IsAbnormal(Outcome::Crashed));
  EXPECT_TRUE(IsAbnormal(Outcome::TimedOut));
  EXPECT_TRUE(IsAbnormal(Outcome::NoProgress));
}
