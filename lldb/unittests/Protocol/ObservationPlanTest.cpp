//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Plugins/Protocol/MCP/ObservationPlan.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <string>
#include <vector>

using namespace lldb_private::mcp;
using ::testing::HasSubstr;
using ::testing::Not;

namespace {

llvm::Expected<ObservationPlan> Parse(llvm::StringRef JSON) {
  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(JSON);
  if (!Parsed)
    return Parsed.takeError();
  return ParseObservationPlan(*Parsed);
}

/// The message a caller would have to act on, or an empty string when the plan
/// was accepted. Tests assert on the prose because an error an agent cannot fix
/// from is as good as no error at all.
std::string Rejection(llvm::StringRef JSON) {
  llvm::Expected<ObservationPlan> Plan = Parse(JSON);
  if (Plan)
    return "";
  return llvm::toString(Plan.takeError());
}

} // namespace

TEST(ObservationPlanTest, DefaultsAreAppliedWhenFieldsAreAbsent) {
  llvm::Expected<ObservationPlan> Plan =
      Parse(R"({"program":"/bin/ls","observe":[{"at":"main"}]})");
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());

  EXPECT_EQ(Plan->Program, "/bin/ls");
  EXPECT_TRUE(Plan->Args.empty());
  EXPECT_TRUE(Plan->Env.empty());
  EXPECT_FALSE(Plan->Cwd.has_value());
  EXPECT_FALSE(Plan->Stdin.has_value());
  EXPECT_TRUE(Plan->CaptureInferiorOutput);
  // A tracepoint's own work is compiled into the program unless asked not to
  // be, since a condition evaluated at a stop is what bounds how hot a
  // tracepoint can be.
  EXPECT_TRUE(Plan->Fast);
  EXPECT_EQ(Plan->TimeoutSeconds, 30u);
  EXPECT_FALSE(Plan->NoProgressSeconds.has_value());

  ASSERT_EQ(Plan->Observations.size(), 1u);
  const Observation &Obs = Plan->Observations.front();
  // An observation with no label is named after the function it observes.
  EXPECT_EQ(Obs.Label, "main");
  EXPECT_EQ(Obs.At, "main");
  EXPECT_FALSE(Obs.OnReturn);
  EXPECT_TRUE(Obs.Capture.empty());
  EXPECT_FALSE(Obs.WhenExpr.has_value());
  EXPECT_FALSE(Obs.CalledFrom.has_value());
  EXPECT_FALSE(Obs.EnabledAfter.has_value());
  EXPECT_EQ(Obs.SkipFirst, 0u);
  EXPECT_FALSE(Obs.OnlyHit.has_value());
  EXPECT_EQ(Obs.Emit, EmitMode::EveryHit);
  EXPECT_EQ(Obs.Backtrace, 0u);
  EXPECT_EQ(Obs.Depth, 2u);
}

TEST(ObservationPlanTest, EveryFieldIsAccepted) {
  llvm::Expected<ObservationPlan> Plan = Parse(R"({
    "program": "/tmp/a.out",
    "args": ["-v", "input.txt"],
    "env": {"LANG": "C"},
    "cwd": "/tmp",
    "stdin": "/tmp/in.txt",
    "capture_inferior_output": false,
    "fast": false,
    "timeout_seconds": 5,
    "no_progress_seconds": 2,
    "observe": [
      {"label": "enter", "at": "main", "on": "entry"},
      {
        "label": "step",
        "at": "Advance",
        "on": "return",
        "capture": ["I->Ops[0]", "Count"],
        "when": "Count > 4000",
        "called_from": "Run",
        "enabled_after": "enter",
        "skip_first": 3,
        "only_hit": 7,
        "emit": "on_change",
        "backtrace": 4,
        "depth": 1
      }
    ]
  })");
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());

  EXPECT_EQ(Plan->Program, "/tmp/a.out");
  EXPECT_EQ(Plan->Args, (std::vector<std::string>{"-v", "input.txt"}));
  EXPECT_EQ(Plan->Env.lookup("LANG"), "C");
  EXPECT_EQ(Plan->Cwd, "/tmp");
  EXPECT_EQ(Plan->Stdin, "/tmp/in.txt");
  EXPECT_FALSE(Plan->CaptureInferiorOutput);
  EXPECT_FALSE(Plan->Fast);
  EXPECT_EQ(Plan->TimeoutSeconds, 5u);
  EXPECT_EQ(Plan->NoProgressSeconds, 2u);

  ASSERT_EQ(Plan->Observations.size(), 2u);
  const Observation &Obs = Plan->Observations.back();
  EXPECT_EQ(Obs.Label, "step");
  EXPECT_EQ(Obs.At, "Advance");
  EXPECT_TRUE(Obs.OnReturn);
  EXPECT_EQ(Obs.Capture, (std::vector<std::string>{"I->Ops[0]", "Count"}));
  EXPECT_EQ(Obs.WhenExpr, "Count > 4000");
  EXPECT_EQ(Obs.CalledFrom, "Run");
  EXPECT_EQ(Obs.EnabledAfter, "enter");
  EXPECT_EQ(Obs.SkipFirst, 3u);
  EXPECT_EQ(Obs.OnlyHit, 7u);
  EXPECT_EQ(Obs.Emit, EmitMode::OnChange);
  EXPECT_EQ(Obs.Backtrace, 4u);
  EXPECT_EQ(Obs.Depth, 1u);
}

TEST(ObservationPlanTest, ObserveAbsentIsCrashTriage) {
  // A plan with nothing to observe still runs the program and reports how it
  // ended, so it must not be mistaken for an incomplete plan.
  llvm::Expected<ObservationPlan> Plan = Parse(R"({"program":"/bin/ls"})");
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
  EXPECT_TRUE(Plan->Observations.empty());

  EXPECT_EQ(Rejection(R"({"program":"/bin/ls","observe":[]})"), "");
}

TEST(ObservationPlanTest, EmptyCaptureIsABareTracepoint) {
  // Recording only hit counts already answers whether code runs at all.
  EXPECT_EQ(Rejection(R"({"program":"p","observe":[{"at":"f"}]})"), "");
  EXPECT_EQ(Rejection(R"({"program":"p","observe":[{"at":"f","capture":[]}]})"),
            "");
}

TEST(ObservationPlanTest, DuplicateLabelsRejected) {
  std::string Message = Rejection(R"({
    "program": "p",
    "observe": [{"label": "loop", "at": "f"}, {"label": "loop", "at": "g"}]
  })");
  EXPECT_THAT(Message, HasSubstr("\"loop\""));
  EXPECT_THAT(Message, HasSubstr("unique"));

  // Two unlabelled observations of one function collide the same way, since a
  // missing label is the function's name.
  EXPECT_THAT(Rejection(R"({
    "program": "p",
    "observe": [{"at": "f"}, {"at": "f", "on": "return"}]
  })"),
              HasSubstr("share the label"));
}

TEST(ObservationPlanTest, EnabledAfterMustNameAKnownLabel) {
  std::string Message = Rejection(R"({
    "program": "p",
    "observe": [{"label": "inner", "at": "f", "enabled_after": "outr"}]
  })");
  EXPECT_THAT(Message, HasSubstr("enabled_after"));
  EXPECT_THAT(Message, HasSubstr("\"outr\""));
  // The labels that do exist are the actionable part of the message.
  EXPECT_THAT(Message, HasSubstr("\"inner\""));
  EXPECT_THAT(Message, HasSubstr("disabled for the whole run"));
}

TEST(ObservationPlanTest, EnabledAfterCannotWaitOnItself) {
  EXPECT_THAT(Rejection(R"({
    "program": "p",
    "observe": [{"label": "a", "at": "f", "enabled_after": "a"}]
  })"),
              HasSubstr("cycle"));
}

TEST(ObservationPlanTest, EnabledAfterCycleRejected) {
  std::string Message = Rejection(R"({
    "program": "p",
    "observe": [
      {"label": "a", "at": "f", "enabled_after": "b"},
      {"label": "b", "at": "g", "enabled_after": "a"}
    ]
  })");
  EXPECT_THAT(Message, HasSubstr("cycle"));
  EXPECT_THAT(Message, HasSubstr("\"a\" -> \"b\" -> \"a\""));
}

TEST(ObservationPlanTest, RawAddressInAtRejected) {
  std::string Message =
      Rejection(R"({"program":"p","observe":[{"at":"0x100003f40"}]})");
  EXPECT_THAT(Message, HasSubstr("0x100003f40"));
  EXPECT_THAT(Message, HasSubstr("address space layout randomization"));
  EXPECT_THAT(Message, HasSubstr("Name the function instead"));

  // A hex address is rejected however it is spelled.
  EXPECT_THAT(Rejection(R"({"program":"p","observe":[{"at":"0XABCDEF"}]})"),
              HasSubstr("address space layout randomization"));

  // A name that merely starts like one is a name.
  EXPECT_EQ(Rejection(R"({"program":"p","observe":[{"at":"0x_handler"}]})"),
            "");
}

TEST(ObservationPlanTest, RawAddressInCalledFromRejected) {
  EXPECT_THAT(Rejection(R"({
    "program": "p",
    "observe": [{"at": "f", "called_from": "0x7fff2010"}]
  })"),
              HasSubstr("address space layout randomization"));
}

TEST(ObservationPlanTest, UnrecognizedPlanFieldRejectedByName) {
  // A field that is dropped silently leaves the caller believing a setting was
  // honoured, so the name and the alternatives both have to be in the message.
  std::string Message = Rejection(R"({"program":"p","timeout":10})");
  EXPECT_THAT(Message, HasSubstr("\"timeout\""));
  EXPECT_THAT(Message, HasSubstr("\"timeout_seconds\""));
}

TEST(ObservationPlanTest, UnrecognizedObservationFieldRejectedByName) {
  std::string Message = Rejection(R"({
    "program": "p",
    "observe": [{"label": "loop", "at": "f", "captur": ["i"]}]
  })");
  EXPECT_THAT(Message, HasSubstr("observation \"loop\""));
  EXPECT_THAT(Message, HasSubstr("\"captur\""));
  EXPECT_THAT(Message, HasSubstr("\"capture\""));
}

TEST(ObservationPlanTest, UnrecognizedFieldsAreListedInOneMessage) {
  // Object iteration order is unspecified, so the report has to be sorted or a
  // caller cannot reproduce it.
  EXPECT_THAT(Rejection(R"({"program":"p","zeta":1,"alpha":2})"),
              HasSubstr("\"alpha\", \"zeta\""));
}

TEST(ObservationPlanTest, ProgramIsRequired) {
  EXPECT_THAT(Rejection(R"({"observe":[]})"), HasSubstr("\"program\""));
  EXPECT_THAT(Rejection(R"({"observe":[]})"), HasSubstr("required"));
}

TEST(ObservationPlanTest, AnEmptyProgramIsNotAMissingOne) {
  // An empty string is *present*, and answering it as absent denied what the
  // caller had just written: told a field it can see in its own request is
  // "required", it goes looking for a second field of that name.
  EXPECT_THAT(Rejection(R"({"program":""})"), HasSubstr("empty string"));
  EXPECT_THAT(Rejection(R"({"program":""})"), Not(HasSubstr("required")));
}

TEST(ObservationPlanTest, AtIsRequired) {
  std::string Message =
      Rejection(R"({"program":"p","observe":[{"label":"loop"}]})");
  EXPECT_THAT(Message, HasSubstr("observation \"loop\""));
  EXPECT_THAT(Message, HasSubstr("\"at\" is required"));
}

TEST(ObservationPlanTest, OnAcceptsOnlyEntryOrReturn) {
  EXPECT_EQ(Rejection(R"({"program":"p","observe":[{"at":"f","on":"entry"}]})"),
            "");
  EXPECT_EQ(
      Rejection(R"({"program":"p","observe":[{"at":"f","on":"return"}]})"), "");

  std::string Message =
      Rejection(R"({"program":"p","observe":[{"at":"f","on":"exit"}]})");
  EXPECT_THAT(Message, HasSubstr("\"exit\""));
  EXPECT_THAT(Message, HasSubstr("\"entry\" or \"return\""));
}

TEST(ObservationPlanTest, EmitAcceptsOnlyTheThreeReductions) {
  auto EmitOf = [](llvm::StringRef Mode) {
    llvm::Expected<ObservationPlan> Plan = Parse(
        (R"({"program":"p","observe":[{"at":"f","emit":")" + Mode + "\"}]}")
            .str());
    if (!Plan) {
      llvm::consumeError(Plan.takeError());
      return std::string("rejected");
    }
    return ToString(Plan->Observations.front().Emit).str();
  };

  EXPECT_EQ(EmitOf("every_hit"), "every_hit");
  EXPECT_EQ(EmitOf("on_change"), "on_change");
  EXPECT_EQ(EmitOf("first_and_last"), "first_and_last");
  EXPECT_EQ(EmitOf("sampled"), "rejected");

  EXPECT_THAT(
      Rejection(R"({"program":"p","observe":[{"at":"f","emit":"sampled"}]})"),
      HasSubstr("\"every_hit\", \"on_change\" or \"first_and_last\""));
}

TEST(ObservationPlanTest, OnlyHitCountsFromOne) {
  EXPECT_THAT(
      Rejection(R"({"program":"p","observe":[{"at":"f","only_hit":0}]})"),
      HasSubstr("counted from 1"));
}

TEST(ObservationPlanTest, TimeoutMustBePositive) {
  EXPECT_THAT(Rejection(R"({"program":"p","timeout_seconds":0})"),
              HasSubstr("at least 1"));
}

TEST(ObservationPlanTest, NoProgressMustBeShorterThanTheTimeout) {
  // A stall detector that cannot fire before the timeout reports nothing.
  EXPECT_THAT(
      Rejection(
          R"({"program":"p","timeout_seconds":10,"no_progress_seconds":10})"),
      HasSubstr("not shorter than"));
  EXPECT_EQ(
      Rejection(
          R"({"program":"p","timeout_seconds":10,"no_progress_seconds":9})"),
      "");
}

TEST(ObservationPlanTest, WronglyTypedFieldsRejected) {
  EXPECT_THAT(Rejection(R"({"program":42})"), HasSubstr("must be a string"));
  EXPECT_THAT(Rejection(R"({"program":"p","args":"-v"})"),
              HasSubstr("must be an array of strings"));
  EXPECT_THAT(Rejection(R"({"program":"p","args":[1]})"),
              HasSubstr("every entry must be a string"));
  EXPECT_THAT(Rejection(R"({"program":"p","env":{"N":1}})"),
              HasSubstr("must be a string"));
  EXPECT_THAT(Rejection(R"({"program":"p","capture_inferior_output":"yes"})"),
              HasSubstr("must be true or false"));
  EXPECT_THAT(Rejection(R"({"program":"p","timeout_seconds":"30"})"),
              HasSubstr("must be a whole number"));
  EXPECT_THAT(Rejection(R"({"program":"p","observe":{"at":"f"}})"),
              HasSubstr("must be an array of observations"));
  EXPECT_THAT(Rejection(R"({"program":"p","observe":["f"]})"),
              HasSubstr("must be an object"));
}

TEST(ObservationPlanTest, AWronglyTypedFieldIsQuotedBackAsWritten) {
  // The field alone does not identify the mistake. A number written as a string
  // makes "must be a whole number" read as a contradiction until the quotes are
  // visible, which is why the value is rendered as JSON rather than described --
  // and `"timeout_seconds": "30"` is a mistake a caller really made.
  EXPECT_THAT(Rejection(R"({"program":"p","timeout_seconds":"30"})"),
              HasSubstr("not \"30\""));
  EXPECT_THAT(Rejection(R"({"program":42})"), HasSubstr("not 42"));
  EXPECT_THAT(Rejection(R"({"program":"p","capture_inferior_output":"yes"})"),
              HasSubstr("not \"yes\""));
  EXPECT_THAT(Rejection(R"({"program":"p","args":"-v"})"),
              HasSubstr("not \"-v\""));
  EXPECT_THAT(Rejection(R"({"program":"p","env":[]})"), HasSubstr("not []"));
  EXPECT_THAT(Rejection(R"({"program":"p","observe":["f"]})"),
              HasSubstr("this one is \"f\""));
}

TEST(ObservationPlanTest, AWrongEntryInAnArrayIsSubscripted) {
  // Which of six entries is at fault is otherwise left to the caller to guess.
  EXPECT_THAT(Rejection(R"({"program":"p","args":["a","b",3]})"),
              HasSubstr("\"args\"[2] is 3"));
  EXPECT_THAT(
      Rejection(
          R"({"program":"p","observe":[{"at":"f","capture":["a","","b"]}]})"),
      HasSubstr("\"capture\"[1] is empty"));
}

TEST(ObservationPlanTest, AQuotedValueDoesNotPutAWholeDocumentInTheMessage) {
  // The value came from the caller, and a plan that put a document where a scalar
  // belonged would otherwise put that document in the error.
  const std::string Message = Rejection(
      R"({"program":"p","timeout_seconds":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa)"
      R"(aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})");
  EXPECT_THAT(Message, HasSubstr("..."));
  EXPECT_LT(Message.size(), 200u) << Message;
}

TEST(ObservationPlanTest, OutOfRangeIntegersRejected) {
  EXPECT_THAT(
      Rejection(R"({"program":"p","observe":[{"at":"f","skip_first":-1}]})"),
      HasSubstr("out of range"));
  EXPECT_THAT(
      Rejection(
          R"({"program":"p","observe":[{"at":"f","backtrace":4294967296}]})"),
      HasSubstr("out of range"));
}

TEST(ObservationPlanTest, EmptyCaptureExpressionRejected) {
  EXPECT_THAT(
      Rejection(R"({"program":"p","observe":[{"at":"f","capture":["  "]}]})"),
      HasSubstr("\"capture\"[0] is empty"));
}

TEST(ObservationPlanTest, NonObjectPlanRejected) {
  EXPECT_THAT(Rejection(R"(["/bin/ls"])"), HasSubstr("must be a JSON object"));
  EXPECT_THAT(Rejection(R"("/bin/ls")"), HasSubstr("must be a JSON object"));
}

TEST(ObservationPlanTest, SourceLocationFormIsSplit) {
  llvm::Expected<ObservationPlan> Plan = Parse(R"({
    "program": "opt",
    "observe": [{"at": "InstCombineAddSub.cpp:1189"}]
  })");
  ASSERT_TRUE(bool(Plan)) << llvm::toString(Plan.takeError());
  ASSERT_EQ(Plan->Observations.size(), 1u);
  EXPECT_EQ(Plan->Observations[0].At, "InstCombineAddSub.cpp");
  ASSERT_TRUE(Plan->Observations[0].AtLine.has_value());
  EXPECT_EQ(*Plan->Observations[0].AtLine, 1189u);
  EXPECT_EQ(Plan->Observations[0].Label, "InstCombineAddSub.cpp:1189");
}

TEST(ObservationPlanTest, QualifiedFunctionNameIsNotASourceLocation) {
  // A trailing colon-and-digits marks a source location, so a qualified name
  // must not be split on its scope operator.
  llvm::Expected<ObservationPlan> Plan = Parse(R"({
    "program": "opt",
    "observe": [{"at": "InstCombinerImpl::visitAdd"}]
  })");
  ASSERT_TRUE(bool(Plan)) << llvm::toString(Plan.takeError());
  ASSERT_EQ(Plan->Observations.size(), 1u);
  EXPECT_EQ(Plan->Observations[0].At, "InstCombinerImpl::visitAdd");
  EXPECT_FALSE(Plan->Observations[0].AtLine.has_value());
}

TEST(ObservationPlanTest, SourceLineCountsFromOne) {
  llvm::Expected<ObservationPlan> Plan = Parse(R"({
    "program": "opt",
    "observe": [{"at": "foo.cpp:0"}]
  })");
  ASSERT_FALSE(bool(Plan));
  EXPECT_NE(llvm::toString(Plan.takeError()).find("count from 1"),
            std::string::npos);
}

//===----------------------------------------------------------------------===//
// Comparing runs
//===----------------------------------------------------------------------===//

TEST(ObservationPlanTest, CompareCarriesTheRunsAndTheirOverrides) {
  llvm::Expected<ObservationPlan> Plan = Parse(R"({
      "program": "/bin/opt",
      "args": ["-S", "in.ll"],
      "compare": [{"label": "fixed"},
                  {"label": "before", "program": "/tmp/opt.before",
                   "args": ["-S", "other.ll"]}]
  })");
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
  ASSERT_EQ(Plan->Compare.size(), 2u);
  EXPECT_EQ(Plan->Compare[0].Label, "fixed");
  EXPECT_FALSE(Plan->Compare[0].Program.has_value());

  // A variant is the plan with fields replaced, so what it does not name it
  // inherits -- which is what keeps the tracepoints and the timeout the same
  // across the runs being compared.
  const ObservationPlan Fixed = Plan->WithVariant(Plan->Compare[0]);
  EXPECT_EQ(Fixed.Program, "/bin/opt");
  EXPECT_EQ(Fixed.Args, std::vector<std::string>({"-S", "in.ll"}));
  EXPECT_TRUE(Fixed.Compare.empty());

  const ObservationPlan Before = Plan->WithVariant(Plan->Compare[1]);
  EXPECT_EQ(Before.Program, "/tmp/opt.before");
  EXPECT_EQ(Before.Args, std::vector<std::string>({"-S", "other.ll"}));
}

TEST(ObservationPlanTest, OneRunIsNotAComparison) {
  EXPECT_THAT(
      Rejection(R"({"program": "/bin/opt", "compare": [{"label": "only"}]})"),
      HasSubstr("nothing to compare it with"));
}

TEST(ObservationPlanTest, RunsNeedLabelsAndTheyHaveToDiffer) {
  EXPECT_THAT(Rejection(R"({"program": "/bin/opt",
      "compare": [{"label": "a"}, {"program": "/tmp/b"}]})"),
              HasSubstr("\"label\" is required"));

  EXPECT_THAT(Rejection(R"({"program": "/bin/opt",
      "compare": [{"label": "a"}, {"label": "a", "program": "/tmp/b"}]})"),
              HasSubstr("labelled \"a\""));
}

TEST(ObservationPlanTest, AVariantMayNotCarryTracepointsOrATimeout) {
  // Varying the observations would produce reports with nothing to line up, and
  // varying the timeout would make the runs incomparable in the dimension a
  // comparison is most often about.
  EXPECT_THAT(Rejection(R"({"program": "/bin/opt",
      "compare": [{"label": "a"}, {"label": "b", "observe": [{"at": "f"}]}]})"),
              HasSubstr("observe"));

  EXPECT_THAT(Rejection(R"({"program": "/bin/opt",
      "compare": [{"label": "a"}, {"label": "b", "timeout_seconds": 90}]})"),
              HasSubstr("timeout_seconds"));
}

TEST(ObservationPlanTest, TooManyRunsIsRefusedWithTheReason) {
  std::string Runs;
  for (size_t I = 0; I <= MaxComparedRuns; ++I) {
    if (I)
      Runs += ",";
    Runs += "{\"label\": \"r" + std::to_string(I) + "\"}";
  }
  EXPECT_THAT(Rejection("{\"program\": \"/bin/opt\", \"compare\": [" + Runs +
                        "]}"),
              HasSubstr("at most"));
}

//===----------------------------------------------------------------------===//
// Source older than the line table it resolved through
//===----------------------------------------------------------------------===//

namespace {

/// A time relative to a fixed instant, so the rule is exercised without a clock.
llvm::sys::TimePoint<> At(std::chrono::seconds Offset) {
  return llvm::sys::TimePoint<>(std::chrono::seconds(1000000000)) + Offset;
}

using Secs = std::chrono::seconds;

} // namespace

TEST(SourceSkewTest, ASourceEditedAfterTheBuildIsReportedWithHowLongAfter) {
  // The failure this exists for is silent by construction: any edit above a
  // traced statement moves it, and the report goes on saying the location
  // resolved, because it did. The note is the only thing that can say otherwise.
  std::optional<std::string> Note =
      DescribeSourceSkew("X86ISelLowering.cpp", At(Secs(600)), At(Secs(0)));
  ASSERT_TRUE(Note.has_value());
  EXPECT_THAT(*Note, HasSubstr("X86ISelLowering.cpp"));
  EXPECT_THAT(*Note, HasSubstr("10m"));
}

TEST(SourceSkewTest, AFreshlyBuiltBinaryIsNewerThanItsSourceAndSaysNothing) {
  // The ordinary case, and it has to be silent: a warning on every run of a
  // healthy tree is a warning nobody reads by the second day.
  EXPECT_FALSE(DescribeSourceSkew("f.cpp", At(Secs(0)), At(Secs(600))));
  EXPECT_FALSE(DescribeSourceSkew("f.cpp", At(Secs(0)), At(Secs(0))));
}

TEST(SourceSkewTest, ASecondApartIsBuildNoiseRatherThanAnEdit) {
  // A build system that touches its inputs, an unpacked archive, or a coarse
  // filesystem timestamp can leave the two within a second either way, and the
  // order then says nothing about whether anyone edited anything.
  EXPECT_FALSE(DescribeSourceSkew("f.cpp", At(Secs(1)), At(Secs(0))));
  EXPECT_TRUE(DescribeSourceSkew("f.cpp", At(Secs(2)), At(Secs(0))));
}

TEST(SourceSkewTest, AnUnreadableMtimeIsNotAFinding) {
  // Debug info records the path the compiler saw, so on a binary built
  // elsewhere the source names a directory this machine does not have. An
  // unreadable mtime arrives as the epoch, and a comparison that cannot be made
  // must not be reported as one that came out badly.
  const llvm::sys::TimePoint<> Missing;
  EXPECT_FALSE(DescribeSourceSkew("f.cpp", Missing, At(Secs(0))));
  EXPECT_FALSE(DescribeSourceSkew("f.cpp", At(Secs(600)), Missing));
  EXPECT_FALSE(DescribeSourceSkew("f.cpp", Missing, Missing));
}

TEST(SourceSkewTest, TheSkewIsScaledSoAnEditReadsDifferentlyFromACheckout) {
  // Minutes is somebody editing between two runs, which is the failure. Days is
  // a tree that was checked out after the binary was built, which usually is
  // not. The fact is the same either way and the number is what separates them,
  // so it has to be legible without arithmetic.
  EXPECT_THAT(*DescribeSourceSkew("f.cpp", At(Secs(30)), At(Secs(0))),
              HasSubstr("30s"));
  EXPECT_THAT(*DescribeSourceSkew("f.cpp", At(Secs(7200)), At(Secs(0))),
              HasSubstr("2h"));
  EXPECT_THAT(*DescribeSourceSkew("f.cpp", At(Secs(864000)), At(Secs(0))),
              HasSubstr("10d"));
}
