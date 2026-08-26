//===-- ComparisonTest.cpp ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// What a comparison of two runs of one plan reports. The value of the feature is
// entirely in what it leaves out -- a caller that gets two reports has to diff
// them itself -- so most of these are about what does *not* appear.
//
//===----------------------------------------------------------------------===//

#include "Plugins/Protocol/MCP/Comparison.h"
#include "Plugins/Protocol/MCP/ObservationEngine.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace lldb_private;
using namespace lldb_private::mcp;

namespace {

std::string Render(const llvm::json::Value &V) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  OS << V;
  return S;
}

/// Rendering an object checks its keys and its contents in one comparison.
std::string Render(const llvm::json::Object &O) {
  return Render(llvm::json::Value(llvm::json::Object(O)));
}

std::string Render(const llvm::json::Array &A) {
  return Render(llvm::json::Value(llvm::json::Array(A)));
}

/// One hit's tuple in the form the engine stores it: the captures joined by the
/// separator, with a trailing one.
std::string Tuple(std::initializer_list<llvm::StringRef> Values) {
  std::string Out;
  for (llvm::StringRef Value : Values) {
    Out += Value.str();
    Out += CaptureTupleSeparator;
  }
  return Out;
}

ObservationResult MakeRun(Outcome How, uint64_t Hits,
                      std::vector<std::string> Tuples,
                      llvm::StringRef Capture = "n") {
  ObservationResult R;
  R.Result = How;
  R.Terminal.Description = How == Outcome::Exited
                               ? "the program ran to completion"
                               : "the program was still running";
  ObservationReport Report;
  Report.Label = "loop";
  Report.At = "loop";
  Report.ResolvedLocations = 1;
  Report.Hits = Hits;
  Report.Emitted = Hits;
  CaptureReport C;
  C.Expr = Capture.str();
  C.Tier = ValueResolutionTier::VariablePath;
  // A tier is set by a resolution that worked, so a capture holding one was
  // evaluated. Left at zero the fixture describes a state no run produces.
  C.Evaluations = Hits;
  Report.Captures.push_back(std::move(C));
  Report.HitTuples = std::move(Tuples);
  R.Observations.push_back(std::move(Report));
  return R;
}

/// The summary an aggregate renders for one capture over a run: a histogram of
/// values with counts, bounded, with the number dropped beside it.
llvm::json::Value Summary(int64_t Distinct,
                          std::initializer_list<std::pair<llvm::StringRef, int64_t>>
                              Values,
                          int64_t Elided) {
  llvm::json::Object Histogram;
  for (const auto &[Value, Count] : Values)
    Histogram[Value] = Count;
  llvm::json::Object Out{{"distinct", Distinct}, {"values", std::move(Histogram)}};
  if (Elided != 0)
    Out["values_elided"] = Elided;
  return Out;
}

std::vector<ComparedRun> Pair(ObservationResult A, ObservationResult B) {
  std::vector<ComparedRun> Runs;
  Runs.push_back({"after", std::move(A), ""});
  Runs.push_back({"before", std::move(B), ""});
  return Runs;
}

const llvm::json::Object *Object(const llvm::json::Value &V,
                                 llvm::StringRef Key) {
  return V.getAsObject()->getObject(Key);
}

} // namespace

TEST(ComparisonTest, TwoIdenticalRunsReportNoDifferenceAndNameWhatMatched) {
  // The question a comparison is asked is whether anything moved. When nothing
  // did, the answer is a list of names -- not two reports and not silence.
  const std::vector<std::string> Tuples = {Tuple({"1"}), Tuple({"2"})};
  const llvm::json::Value Out = CompareRuns(
      Pair(MakeRun(Outcome::Exited, 2, Tuples), MakeRun(Outcome::Exited, 2, Tuples)));

  EXPECT_EQ(Out.getAsObject()->get("diverged"), nullptr) << Render(Out);
  EXPECT_EQ(Out.getAsObject()->get("first_divergent_hit"), nullptr)
      << Render(Out);
  const llvm::json::Array *Agreed = Out.getAsObject()->getArray("agreed");
  ASSERT_NE(Agreed, nullptr);
  EXPECT_EQ(Render(*Agreed),
            R"(["outcome","ended","loop.resolved_locations","loop.hits",)"
            R"("loop.emitted"])");
}

TEST(ComparisonTest, WhatDivergedCarriesBothSidesAndIsNotAlsoCalledAgreed) {
  const llvm::json::Value Out =
      CompareRuns(Pair(MakeRun(Outcome::Exited, 2, {Tuple({"1"}), Tuple({"2"})}),
                       MakeRun(Outcome::TimedOut, 9, {Tuple({"1"}), Tuple({"2"})})));

  const llvm::json::Object *Diverged = Object(Out, "diverged");
  ASSERT_NE(Diverged, nullptr) << Render(Out);
  EXPECT_EQ(Render(*Diverged->getObject("outcome")),
            R"({"after":"exited","before":"timed_out"})");
  EXPECT_EQ(Render(*Diverged->getObject("loop.hits")),
            R"({"after":"2","before":"9"})");

  const std::string Agreed =
      Render(*Out.getAsObject()->getArray("agreed"));
  EXPECT_EQ(Agreed.find("outcome"), std::string::npos) << Agreed;
  EXPECT_NE(Agreed.find("loop.resolved_locations"), std::string::npos) << Agreed;
}

TEST(ComparisonTest, TwoRunsDifferingOnlyInExitStatusDoNotAgree) {
  // A pass/fail pair is the commonest before-and-after there is, and the outcome,
  // the description and every count are identical across one: the status is the
  // whole of the difference, so omitting it reported the pair as no difference.
  ObservationResult A = MakeRun(Outcome::Exited, 2, {Tuple({"1"}), Tuple({"2"})});
  ObservationResult B = A;
  A.Terminal.ExitStatus = 0;
  B.Terminal.ExitStatus = 1;

  const llvm::json::Value Out = CompareRuns(Pair(std::move(A), std::move(B)));
  const llvm::json::Object *Diverged = Object(Out, "diverged");
  ASSERT_NE(Diverged, nullptr) << Render(Out);
  EXPECT_EQ(Render(*Diverged->getObject("exit_status")),
            R"({"after":"0","before":"1"})");
}

TEST(ComparisonTest, WhereARunEndedIsALineAndNotJustAFunction) {
  // A crashed run that reports a signal and a function name and no line leaves
  // the caller to spend a second call finding out where. The directory goes: two
  // builds of one source differ in every character of it and in nothing else.
  ObservationResult A = MakeRun(Outcome::Crashed, 2, {Tuple({"1"}), Tuple({"2"})});
  ObservationResult B = A;
  A.Terminal.File = "/tmp/build-a/drift.c";
  A.Terminal.Line = 41;
  B.Terminal.File = "/tmp/build-b/drift.c";
  B.Terminal.Line = 52;

  const llvm::json::Value Out = CompareRuns(Pair(std::move(A), std::move(B)));
  EXPECT_EQ(Render(*Object(Out, "diverged")->getObject("ended_at")),
            R"({"after":"drift.c:41","before":"drift.c:52"})");
}

TEST(ComparisonTest, ARunWithNoLineToReportDoesNotGetAnEmptyOne) {
  // A program that exited normally was never anywhere in particular, and a row
  // reading `:0` is a name in the report costing bytes to say nothing.
  const std::vector<std::string> Tuples = {Tuple({"1"})};
  const llvm::json::Value Out = CompareRuns(
      Pair(MakeRun(Outcome::Exited, 1, Tuples), MakeRun(Outcome::Exited, 1, Tuples)));

  const std::string Agreed = Render(*Out.getAsObject()->getArray("agreed"));
  EXPECT_EQ(Agreed.find("ended_at"), std::string::npos) << Agreed;
  EXPECT_EQ(Agreed.find("exit_status"), std::string::npos) << Agreed;
}

TEST(ComparisonTest, HowLongARunTookIsWholeMilliseconds) {
  // A double goes into the document at max_digits10, so 358.432 is printed as
  // 358.43200000000002: thirteen characters per row of a response that is charged
  // for its size on every later turn, spent on precision the measurement does not
  // have. The same binary under the same plan measured 182 ms and 558 ms.
  ObservationResult A = MakeRun(Outcome::Exited, 1, {Tuple({"1"})});
  ObservationResult B = A;
  A.ElapsedMs = 358.432;
  B.ElapsedMs = 0.4;

  const llvm::json::Value Out = CompareRuns(Pair(std::move(A), std::move(B)));
  const llvm::json::Array *Rows = Out.getAsObject()->getArray("runs");
  ASSERT_NE(Rows, nullptr);
  EXPECT_EQ((*Rows)[0].getAsObject()->getInteger("elapsed_ms"),
            std::optional<int64_t>(358));
  EXPECT_EQ(Render(*Rows),
            R"([{"elapsed_ms":358,"ended":"the program ran to completion",)"
            R"("hits":{"loop":1},"label":"after","outcome":"exited"},)"
            R"({"elapsed_ms":0,"ended":"the program ran to completion",)"
            R"("hits":{"loop":1},"label":"before","outcome":"exited"}])");
}

TEST(ComparisonTest, HowACaptureResolvedIsComparedAndWhatItCostIsNot) {
  // A capture reports itself as a cost account, and a timing in a comparison makes
  // every expression-tier capture diverge on the clock alone -- measured at 165 of
  // 803 chars of one response, two slightly different timings under a name a
  // reader takes for the value of the expression.
  ObservationResult A = MakeRun(Outcome::Exited, 1, {Tuple({"1"})});
  ObservationResult B = A;
  for (ObservationResult *R : {&A, &B}) {
    CaptureReport &C = R->Observations.front().Captures.front();
    C.Tier = ValueResolutionTier::Expression;
    C.TotalMs = R == &A ? 599.88 : 632.14;
  }

  const llvm::json::Value Out = CompareRuns(Pair(std::move(A), std::move(B)));
  EXPECT_EQ(Out.getAsObject()->get("diverged"), nullptr) << Render(Out);
  // Which mechanism read the value is still compared: a capture read through the
  // expression evaluator in one run and off a variable path in the other is a
  // difference in what was measured.
  const std::string Agreed = Render(*Out.getAsObject()->getArray("agreed"));
  EXPECT_NE(Agreed.find("loop.n"), std::string::npos) << Agreed;
}

TEST(ComparisonTest, ACaptureThatFailedInBothRunsIsNotADivergence) {
  // 431 chars of one response were a capture that failed the same way twice,
  // reported as a difference because the two runs failed at it a different number
  // of times.
  ObservationResult A = MakeRun(Outcome::Exited, 4, {Tuple({"1"})});
  ObservationResult B = MakeRun(Outcome::Exited, 9, {Tuple({"1"})});
  for (ObservationResult *R : {&A, &B}) {
    CaptureReport &C = R->Observations.front().Captures.front();
    C.Tier = ValueResolutionTier::Unresolved;
    C.Errors = C.Evaluations;
  }

  const llvm::json::Value Out = CompareRuns(Pair(std::move(A), std::move(B)));
  const llvm::json::Object *Diverged = Object(Out, "diverged");
  ASSERT_NE(Diverged, nullptr) << Render(Out);
  EXPECT_EQ(Diverged->get("loop.n"), nullptr) << Render(Out);
  const std::string Agreed = Render(*Out.getAsObject()->getArray("agreed"));
  EXPECT_NE(Agreed.find("loop.n"), std::string::npos) << Agreed;
}

TEST(ComparisonTest, TheDottedNameHoldsTheValueAndTheMetadataSitsUnderIt) {
  // The row carrying the answer -- 320 against 282 -- was named `<label> summary
  // of <expr>`, a phrase that cannot be addressed as a path and sorts away from
  // the siblings it belongs beside, while the dotted name a reader would guess
  // held metadata about the capture.
  ObservationResult A = MakeRun(Outcome::Exited, 1, {Tuple({"320"})});
  ObservationResult B = MakeRun(Outcome::Exited, 1, {Tuple({"282"})});
  A.Aggregate = llvm::json::Object{{"loop", llvm::json::Object{{"n", "320 x1"}}}};
  B.Aggregate = llvm::json::Object{{"loop", llvm::json::Object{{"n", "282 x1"}}}};
  for (ObservationResult *R : {&A, &B})
    R->Observations.front().Captures.front().Tier =
        ValueResolutionTier::Expression;

  const llvm::json::Value Out = CompareRuns(Pair(std::move(A), std::move(B)));
  const llvm::json::Object *Diverged = Object(Out, "diverged");
  ASSERT_NE(Diverged, nullptr) << Render(Out);
  EXPECT_NE(Diverged->get("loop.n"), nullptr) << Render(Out);

  // And how the capture resolved, which both runs agreed on, is named beneath the
  // value rather than in place of it.
  const std::string Agreed = Render(*Out.getAsObject()->getArray("agreed"));
  EXPECT_NE(Agreed.find("loop.n.capture"), std::string::npos) << Agreed;
  EXPECT_EQ(Agreed.find("summary of"), std::string::npos) << Agreed;
}

TEST(ComparisonTest, AComparedValueKeepsItsShapeInsteadOfBeingEscaped) {
  // A value serialized into a string comes back double-quoted and a document comes
  // back as a line of backslashes for the caller to undo by hand -- which is the
  // complaint the capture aggregate exists to answer, reintroduced one level up.
  ObservationResult A = MakeRun(Outcome::Exited, 1, {Tuple({"320"})});
  ObservationResult B = MakeRun(Outcome::Exited, 1, {Tuple({"282"})});
  A.Aggregate = llvm::json::Object{{"loop", llvm::json::Object{{"n", "320 x1"}}}};
  B.Aggregate = llvm::json::Object{{"loop", llvm::json::Object{{"n", "282 x1"}}}};

  const llvm::json::Value Out = CompareRuns(Pair(std::move(A), std::move(B)));
  EXPECT_EQ(Render(*Object(Out, "diverged")->getObject("loop.n")),
            R"({"after":"320 x1","before":"282 x1"})");
}

TEST(ComparisonTest, TwoHistogramsAreShortenedToTheValuesTheyDisagreeOn) {
  // A summary is bounded on its own, but two of them side by side in a document
  // that exists to be short is a different budget: a measured pair was two
  // 260-char histograms of twenty-eight values whose real difference was one of
  // them. The rest is the part of the run they have in common, which nobody asked
  // for twice.
  ObservationResult A = MakeRun(Outcome::Exited, 30, {Tuple({"1"})});
  ObservationResult B = A;
  A.Aggregate = llvm::json::Object{
      {"loop",
       llvm::json::Object{{"n", Summary(28, {{"0", 2}, {"14", 1}, {"184", 2}}, 20)}}}};
  B.Aggregate = llvm::json::Object{
      {"loop",
       llvm::json::Object{{"n", Summary(28, {{"0", 2}, {"14", 1}, {"198", 2}}, 20)}}}};

  const llvm::json::Value Out = CompareRuns(Pair(std::move(A), std::move(B)));
  EXPECT_EQ(Render(*Object(Out, "diverged")->getObject("loop.n")),
            R"({"after":{"distinct":28,"values":{"184":2},"values_elided":20},)"
            R"("before":{"distinct":28,"values":{"198":2},"values_elided":20}})");
}

TEST(ComparisonTest, ALongValueIsShortenedWithoutLeavingAnUnparseableDocument) {
  // Slicing the serialization produced `{"short":"{\"distinct\":12,...,\"..."`: a
  // document cut mid-token, which nothing can parse and which reads as though the
  // value itself ended there.
  const std::string Long(MaxComparedSummaryChars + 200, 'x');
  ObservationResult A = MakeRun(Outcome::Exited, 1, {Tuple({"1"})});
  ObservationResult B = A;
  A.Aggregate = llvm::json::Object{
      {"loop", llvm::json::Object{{"n", llvm::json::Object{{"value", Long},
                                                           {"count", 1}}}}}};
  B.Aggregate =
      llvm::json::Object{{"loop", llvm::json::Object{{"n", "short x1"}}}};

  const llvm::json::Value Out = CompareRuns(Pair(std::move(A), std::move(B)));
  const llvm::json::Object *Diverged = Object(Out, "diverged")->getObject("loop.n");
  ASSERT_NE(Diverged, nullptr) << Render(Out);
  // Still a document, with the text inside it bounded rather than the document cut.
  const llvm::json::Object *Shown = Diverged->getObject("after");
  ASSERT_NE(Shown, nullptr) << Render(Out);
  EXPECT_EQ(Shown->getInteger("count"), std::optional<int64_t>(1));
  EXPECT_EQ(Shown->getString("value")->size(), MaxComparedSummaryChars + 3);
}

TEST(ComparisonTest, TwoRunsThatPrintedTheSameThingCostOneName) {
  // The streams are compared as their whole text, so identical output collapses to
  // a name. Putting two 8 kB streams in the document would be most of a response
  // that is charged for its size on every later turn.
  ObservationResult A = MakeRun(Outcome::Exited, 1, {Tuple({"1"})});
  ObservationResult B = A;
  A.Output.Out = "total=320 groups=39\r\n";
  B.Output.Out = "total=320 groups=39\r\n";

  const llvm::json::Value Out = CompareRuns(Pair(std::move(A), std::move(B)));
  EXPECT_EQ(Out.getAsObject()->get("diverged"), nullptr) << Render(Out);
  const std::string Agreed = Render(*Out.getAsObject()->getArray("agreed"));
  EXPECT_NE(Agreed.find("stdout"), std::string::npos) << Agreed;
}

TEST(ComparisonTest, TheLineTwoRunsOutputPartsCompanyOnIsTheDifference) {
  // For a miscompile the wrong answer usually *is* stdout, and it was collected on
  // every compared run and then read by nothing. The carriage returns are the
  // pseudo-terminal's, not the program's, so they are gone before the line is
  // compared or shown.
  ObservationResult A = MakeRun(Outcome::Exited, 1, {Tuple({"1"})});
  ObservationResult B = A;
  A.Output.Out = "reading in.ll\r\ntotal=320 groups=39\r\n";
  B.Output.Out = "reading in.ll\r\ntotal=282 groups=9\r\n";

  const llvm::json::Value Out = CompareRuns(Pair(std::move(A), std::move(B)));
  EXPECT_EQ(Render(*Object(Out, "diverged")->getObject("stdout")),
            R"({"after":"total=320 groups=39","before":"total=282 groups=9",)"
            R"("first_differing_line":2})");
}

TEST(ComparisonTest, OutputInOneRunAndSilenceInTheOtherIsADifference) {
  // A program that stopped printing is the difference being looked for, and a long
  // line of it is still one line: the stream is bounded at 8 kB and the whole of it
  // is never what a caller reads.
  ObservationResult A = MakeRun(Outcome::Exited, 1, {Tuple({"1"})});
  ObservationResult B = A;
  A.Output.Err = std::string(MaxComparedOutputChars + 50, 'x') + "\n";

  const llvm::json::Value Out = CompareRuns(Pair(std::move(A), std::move(B)));
  const llvm::json::Object *Stderr = Object(Out, "diverged")->getObject("stderr");
  ASSERT_NE(Stderr, nullptr) << Render(Out);
  EXPECT_EQ(Stderr->getInteger("first_differing_line"),
            std::optional<int64_t>(1));
  EXPECT_EQ(Stderr->getString("after")->size(), MaxComparedOutputChars + 3);
  // And the run that printed nothing is not a side of it, the same way a run
  // missing any other compared thing is not.
  EXPECT_EQ(Stderr->get("before"), nullptr) << Render(Out);
}

TEST(ComparisonTest, ACaptureOnlyOneRunCouldReadIsNamedWithTheRunsThatFailed) {
  // "Resolved in one run and not the other" is the commonest difference there is,
  // and it reached the caller only as an escaped error inside `saw`, and only if
  // that hit happened to be the divergent one.
  ObservationResult A = MakeRun(Outcome::Exited, 2, {Tuple({"1"}), Tuple({"2"})});
  ObservationResult B = A;
  CaptureFailureReport Failure;
  Failure.Label = "loop";
  Failure.Expr = "i";
  Failure.Kind = CaptureFailure::UnknownName;
  Failure.Reason = "use of undeclared identifier 'i'\n    i\n    ^";
  // Two runs' lists of what was in scope instead are the same list twice.
  Failure.Candidates.Names = {"index", "n"};
  Failure.Candidates.InScope = 40;
  B.CaptureFailures.push_back(std::move(Failure));

  const llvm::json::Value Out = CompareRuns(Pair(std::move(A), std::move(B)));
  const llvm::json::Array *Failures =
      Out.getAsObject()->getArray("capture_failures");
  ASSERT_NE(Failures, nullptr) << Render(Out);
  ASSERT_EQ(Failures->size(), 1u);
  EXPECT_EQ(Render(*Failures),
            R"([{"capture":"i","detail":"use of undeclared identifier 'i'",)"
            R"("in":["before"],"observation":"loop","reason":"no_such_name"}])");
}

TEST(ComparisonTest, AFaultEveryRunHitIsReportedOnceAndNotPerRun) {
  // A capture that fails in every run is a fault in the request rather than a
  // difference between the runs, so it is neither repeated nor labelled.
  ObservationResult A = MakeRun(Outcome::Exited, 2, {Tuple({"1"}), Tuple({"2"})});
  CaptureFailureReport Failure;
  Failure.Label = "loop";
  Failure.Expr = "i";
  Failure.Kind = CaptureFailure::UnknownName;
  A.CaptureFailures.push_back(Failure);
  A.Notes.push_back("the artifact was truncated");
  ObservationResult B = A;

  const llvm::json::Value Out = CompareRuns(Pair(std::move(A), std::move(B)));
  EXPECT_EQ(Render(*Out.getAsObject()->getArray("capture_failures")),
            R"([{"capture":"i","observation":"loop","reason":"no_such_name"}])");
  EXPECT_EQ(Render(*Out.getAsObject()->getArray("notes")),
            R"(["the artifact was truncated"])");
}

TEST(ComparisonTest, ANoteOnlySomeRunsMadeCarriesTheRunsThatMadeIt) {
  ObservationResult A = MakeRun(Outcome::Exited, 2, {Tuple({"1"}), Tuple({"2"})});
  ObservationResult B = A;
  B.Notes.push_back("sampling was stopped to keep within the run's budget");

  const llvm::json::Value Out = CompareRuns(Pair(std::move(A), std::move(B)));
  EXPECT_EQ(Render(*Out.getAsObject()->getArray("notes")),
            R"([{"in":["before"],)"
            R"("note":"sampling was stopped to keep within the run's budget"}])");
}

TEST(ComparisonTest, TheFirstHitTheyDisagreeOnIsReportedWithWhatEachSaw) {
  // For a miscompile this is the answer: everything before it is the same
  // computation, and everything after is a consequence of this.
  const llvm::json::Value Out = CompareRuns(
      Pair(MakeRun(Outcome::Exited, 4,
               {Tuple({"1"}), Tuple({"2"}), Tuple({"32"}), Tuple({"4"})}),
           MakeRun(Outcome::Exited, 4,
               {Tuple({"1"}), Tuple({"2"}), Tuple({"64"}), Tuple({"4"})})));

  const llvm::json::Object *First = Object(Out, "first_divergent_hit");
  ASSERT_NE(First, nullptr) << Render(Out);
  const llvm::json::Object *Loop = First->getObject("loop");
  ASSERT_NE(Loop, nullptr);
  // Counted from one, the way every other hit number in a report is.
  EXPECT_EQ(Loop->getInteger("hit"), std::optional<int64_t>(3));
  // Keyed by the capture, not handed back as the separator-joined tuple the
  // engine compares internally.
  EXPECT_EQ(Render(*Loop->getObject("saw")),
            R"({"after":{"n":"32"},"before":{"n":"64"}})");
}

TEST(ComparisonTest, EveryRunIsComparedAgainstTheBaselineAndNotJustTheLast) {
  // Comparing the first run with the last skipped everything in between, so a
  // three-run plan whose middle run was the wrong one reported no divergent hit at
  // all and the wrong run survived as one changed histogram bucket.
  std::vector<ComparedRun> Runs;
  Runs.push_back({"good",
                  MakeRun(Outcome::Exited, 3,
                          {Tuple({"1"}), Tuple({"2"}), Tuple({"3"})}),
                  ""});
  Runs.push_back({"bad",
                  MakeRun(Outcome::Exited, 3,
                          {Tuple({"1"}), Tuple({"9"}), Tuple({"3"})}),
                  ""});
  Runs.push_back({"also-good",
                  MakeRun(Outcome::Exited, 3,
                          {Tuple({"1"}), Tuple({"2"}), Tuple({"3"})}),
                  ""});

  const llvm::json::Value Out = CompareRuns(Runs);
  const llvm::json::Object *Loop =
      Object(Out, "first_divergent_hit")->getObject("loop");
  ASSERT_NE(Loop, nullptr) << Render(Out);
  EXPECT_EQ(Loop->getInteger("hit"), std::optional<int64_t>(2));
  // The baseline and the run that differed from it. The third run saw what the
  // baseline saw, and listing it would read as three-way disagreement about a hit
  // two of them agreed on.
  EXPECT_EQ(Render(*Loop->getObject("saw")),
            R"({"bad":{"n":"9"},"good":{"n":"2"}})");
}

TEST(ComparisonTest, TheBaselineIsTheFirstRunThatProducedAResult) {
  // The field was skipped entirely when the first run failed to launch, so the
  // documented answer went missing for the case a comparison is most often made to
  // investigate: a change that stops the program from starting.
  std::vector<ComparedRun> Runs;
  Runs.push_back({"missing", std::nullopt, "'/tmp/gone' does not exist"});
  Runs.push_back({"good",
                  MakeRun(Outcome::Exited, 2, {Tuple({"1"}), Tuple({"2"})}), ""});
  Runs.push_back({"bad",
                  MakeRun(Outcome::Exited, 2, {Tuple({"1"}), Tuple({"9"})}), ""});

  const llvm::json::Value Out = CompareRuns(Runs);
  const llvm::json::Object *Loop =
      Object(Out, "first_divergent_hit")->getObject("loop");
  ASSERT_NE(Loop, nullptr) << Render(Out);
  EXPECT_EQ(Loop->getInteger("hit"), std::optional<int64_t>(2));
  EXPECT_EQ(Render(*Loop->getObject("saw")),
            R"({"bad":{"n":"9"},"good":{"n":"2"}})");
}

TEST(ComparisonTest, OneRunBeingLongerIsNotADisagreementAboutAnyHit) {
  // Two runs of a program racing a clock differ in how far they got, which is
  // timing rather than behaviour. Reporting a divergence would name a hit at
  // which nothing in fact disagreed.
  const llvm::json::Value Out =
      CompareRuns(Pair(MakeRun(Outcome::TimedOut, 3,
                           {Tuple({"1"}), Tuple({"2"}), Tuple({"3"})}),
                       MakeRun(Outcome::TimedOut, 2, {Tuple({"1"}), Tuple({"2"})})));

  const llvm::json::Object *Loop = Object(Out, "first_divergent_hit")->getObject(
      "loop");
  ASSERT_NE(Loop, nullptr) << Render(Out);
  EXPECT_EQ(Loop->get("hit"), nullptr);
  EXPECT_EQ(Loop->getInteger("identical_through"), std::optional<int64_t>(2));
}

TEST(ComparisonTest, AgreementIsNotClaimedPastWhatWasKept) {
  // "The runs agreed" must not be able to mean "they agreed as far as anything
  // was kept for comparing".
  ObservationResult A = MakeRun(Outcome::Exited, 9000, {Tuple({"1"})});
  ObservationResult B = MakeRun(Outcome::Exited, 9000, {Tuple({"1"})});
  A.Observations.front().HitTuplesDropped = 4904;
  B.Observations.front().HitTuplesDropped = 4904;

  // Bound before it is walked: getObject hands back a pointer into the value, and
  // a temporary is gone by the time the pointer is read.
  const llvm::json::Value Out = CompareRuns(Pair(std::move(A), std::move(B)));
  const llvm::json::Object *Loop =
      Object(Out, "first_divergent_hit")->getObject("loop");
  ASSERT_NE(Loop, nullptr) << Render(Out);
  EXPECT_EQ(Loop->getInteger("identical_through"), std::optional<int64_t>(1));
  EXPECT_NE(Loop->getString("beyond_that"), std::nullopt);
}

TEST(ComparisonTest, ARunThatCouldNotBeMadeIsARowRatherThanAnError) {
  // A change that stops the program from starting is the difference being looked
  // for, so the other runs still have to answer.
  std::vector<ComparedRun> Runs;
  Runs.push_back({"ok", MakeRun(Outcome::Exited, 2, {Tuple({"1"}), Tuple({"2"})}),
                  ""});
  Runs.push_back({"missing", std::nullopt, "'/tmp/gone' does not exist"});

  const llvm::json::Value Out = CompareRuns(Runs);
  const llvm::json::Array *Rows = Out.getAsObject()->getArray("runs");
  ASSERT_NE(Rows, nullptr);
  ASSERT_EQ(Rows->size(), 2u);
  EXPECT_EQ((*Rows)[1].getAsObject()->getString("error"),
            std::optional<llvm::StringRef>("'/tmp/gone' does not exist"));
}

TEST(ComparisonTest, ARunWithNoResultIsNotInTheDenominatorOfAgreement) {
  // Agreement is decided against the runs that answered. Counting the run that
  // could not be launched would leave every row one short of the total, so every
  // row would report as a one-sided divergence and the whole of the surviving
  // run's report would come back under `diverged` -- which is the opposite of
  // the other runs still answering. That is what this rules out.
  //
  // Nothing comes back under `agreed` either, and for a different reason: with
  // one run answering there is nothing for its rows to have agreed with. The two
  // absences are not the same claim, and only the first one is about the
  // denominator.
  std::vector<ComparedRun> Runs;
  Runs.push_back({"ok", MakeRun(Outcome::Exited, 2, {Tuple({"1"}), Tuple({"2"})}),
                  ""});
  Runs.push_back({"missing", std::nullopt, "'/tmp/gone' does not exist"});

  const llvm::json::Value Out = CompareRuns(Runs);
  EXPECT_EQ(Out.getAsObject()->get("diverged"), nullptr) << Render(Out);
  EXPECT_EQ(Out.getAsObject()->get("agreed"), nullptr) << Render(Out);

  // And the run that did answer still answers: its row carries its outcome and
  // its hits, which is the whole point of comparing against a run that failed.
  const llvm::json::Array *Rows = Out.getAsObject()->getArray("runs");
  ASSERT_NE(Rows, nullptr) << Render(Out);
  ASSERT_EQ(Rows->size(), 2u) << Render(Out);
  const llvm::json::Object *Answered = (*Rows)[0].getAsObject();
  ASSERT_NE(Answered, nullptr) << Render(Out);
  EXPECT_EQ(Answered->getString("label"), "ok");
  EXPECT_EQ(Answered->getString("outcome"), "exited");
  const llvm::json::Object *Missing = (*Rows)[1].getAsObject();
  ASSERT_NE(Missing, nullptr) << Render(Out);
  EXPECT_EQ(Missing->getString("label"), "missing");
  EXPECT_NE(Missing->get("error"), nullptr) << Render(Out);
}

TEST(ComparisonTest, AnObservationOnlyOneRunResolvedIsADifference) {
  // The commonest difference between two binaries: a function one of them does
  // not have. Silence about it would read as agreement.
  ObservationResult A = MakeRun(Outcome::Exited, 2, {Tuple({"1"}), Tuple({"2"})});
  ObservationResult B = A;
  B.Observations.clear();

  const llvm::json::Value Out = CompareRuns(Pair(std::move(A), std::move(B)));
  const llvm::json::Object *Diverged = Object(Out, "diverged");
  ASSERT_NE(Diverged, nullptr) << Render(Out);
  const llvm::json::Object *Hits = Diverged->getObject("loop.hits");
  ASSERT_NE(Hits, nullptr);
  EXPECT_EQ(Hits->getString("after"), std::optional<llvm::StringRef>("2"));
  EXPECT_EQ(Hits->get("before"), nullptr);
}
