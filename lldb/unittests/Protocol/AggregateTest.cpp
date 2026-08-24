//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Plugins/Protocol/MCP/Aggregate.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace lldb_private::mcp;

namespace {

std::string ToString(const llvm::json::Value &V) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  OS << V;
  return S;
}

/// Renders an array as text, which checks its order and its contents in one
/// comparison.
std::string ToString(const llvm::json::Array &A) {
  return ToString(llvm::json::Value(llvm::json::Array(A)));
}

std::string ToString(const llvm::json::Object &O) {
  return ToString(llvm::json::Value(llvm::json::Object(O)));
}

/// The rendered summary for one capture, or null when the aggregate holds none.
const llvm::json::Value *FindCapture(const llvm::json::Value &Summary,
                                     llvm::StringRef Label,
                                     llvm::StringRef Capture) {
  const llvm::json::Object *Labels = Summary.getAsObject();
  if (!Labels)
    return nullptr;
  const llvm::json::Object *Captures = Labels->getObject(Label);
  if (!Captures)
    return nullptr;
  return Captures->get(Capture);
}

/// A tail of the event stream holding \p Block repeated \p Times over.
std::vector<std::string> Repeat(llvm::ArrayRef<llvm::StringRef> Block,
                                unsigned Times) {
  std::vector<std::string> Tail;
  for (unsigned I = 0; I < Times; ++I)
    for (llvm::StringRef Label : Block)
      Tail.push_back(Label.str());
  return Tail;
}

} // namespace

TEST(AggregateTest, NothingRecordedRendersAnEmptyObject) {
  Aggregator Aggregate;
  EXPECT_EQ(ToString(Aggregate.Render()), "{}");
}

TEST(AggregateTest, SingleValuedCaptureCollapsesToAString) {
  Aggregator Aggregate;
  for (uint64_t Seq = 0; Seq < 4012; ++Seq)
    Aggregate.Record("loop", "done", "false", Seq, Seq);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Value *Entry = FindCapture(Summary, "loop", "done");
  ASSERT_NE(Entry, nullptr);
  ASSERT_TRUE(Entry->getAsString().has_value());
  EXPECT_EQ(Entry->getAsString()->str(), "false x4012");
}

TEST(AggregateTest, RepeatedValuesAreCountedRatherThanCollapsed) {
  // The events for repeated values are what an emission mode drops, and the
  // sequence numbers here are what such a run would hand over. The aggregate
  // still has to count every hit, since it is the only thing that sees them
  // all.
  Aggregator Aggregate;
  Aggregate.Record("loop", "n", "7", 0, 0);
  Aggregate.Record("loop", "n", "7", 100, 100);
  Aggregate.Record("loop", "n", "7", 250, 250);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Value *Entry = FindCapture(Summary, "loop", "n");
  ASSERT_NE(Entry, nullptr);
  ASSERT_TRUE(Entry->getAsString().has_value());
  EXPECT_EQ(Entry->getAsString()->str(), "7 x3");
}

TEST(AggregateTest, MultiValuedCaptureRendersAnObject) {
  Aggregator Aggregate;
  Aggregate.Record("loop", "state", "idle", 0, 0);
  Aggregate.Record("loop", "state", "idle", 1, 1);
  Aggregate.Record("loop", "state", "busy", 2, 2);
  Aggregate.Record("loop", "state", "idle", 3, 3);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Value *Entry = FindCapture(Summary, "loop", "state");
  ASSERT_NE(Entry, nullptr);
  const llvm::json::Object *Fields = Entry->getAsObject();
  ASSERT_NE(Fields, nullptr);

  EXPECT_EQ(Fields->getInteger("distinct"), std::optional<int64_t>(2));

  const llvm::json::Object *Values = Fields->getObject("values");
  ASSERT_NE(Values, nullptr);
  EXPECT_EQ(Values->getInteger("idle"), std::optional<int64_t>(3));
  EXPECT_EQ(Values->getInteger("busy"), std::optional<int64_t>(1));

  const llvm::json::Array *Transitions = Fields->getArray("transitions");
  ASSERT_NE(Transitions, nullptr);
  EXPECT_EQ(ToString(*Transitions), R"([{"from":"idle","seq":2,"to":"busy"},)"
                                    R"({"from":"busy","seq":3,"to":"idle"}])");

  // Four observations are too few for a rare value to mean anything.
  EXPECT_EQ(Fields->get("outliers"), nullptr);
}

TEST(AggregateTest, TransitionsCoverOnlyChangesAndStayInSequenceOrder) {
  Aggregator Aggregate;
  const llvm::StringRef Values[] = {"a", "a", "b", "b", "b", "c", "a"};
  uint64_t Seq = 0;
  for (llvm::StringRef Value : Values) {
    Aggregate.Record("loop", "phase", Value, Seq, Seq);
    ++Seq;
  }

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Value *Entry = FindCapture(Summary, "loop", "phase");
  ASSERT_NE(Entry, nullptr);
  const llvm::json::Object *Fields = Entry->getAsObject();
  ASSERT_NE(Fields, nullptr);

  const llvm::json::Array *Transitions = Fields->getArray("transitions");
  ASSERT_NE(Transitions, nullptr);
  EXPECT_EQ(ToString(*Transitions), R"([{"from":"a","seq":2,"to":"b"},)"
                                    R"({"from":"b","seq":5,"to":"c"},)"
                                    R"({"from":"c","seq":6,"to":"a"}])");
}

TEST(AggregateTest, OutliersReportValuesSeenOnceOrTwiceAmongMany) {
  Aggregator Aggregate;
  uint64_t Seq = 0;
  auto RecordCommon = [&](unsigned Times) {
    for (unsigned I = 0; I < Times; ++I) {
      Aggregate.Record("loop", "kind", "common", Seq, Seq);
      ++Seq;
    }
  };

  RecordCommon(10);
  Aggregate.Record("loop", "kind", "twice", Seq, Seq);
  ++Seq;
  RecordCommon(5);
  Aggregate.Record("loop", "kind", "twice", Seq, Seq);
  ++Seq;
  RecordCommon(5);
  Aggregate.Record("loop", "kind", "once", Seq, Seq);
  ++Seq;
  RecordCommon(2);
  ASSERT_GE(Seq, Aggregator::MinObservationsForOutliers);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Value *Entry = FindCapture(Summary, "loop", "kind");
  ASSERT_NE(Entry, nullptr);
  const llvm::json::Object *Fields = Entry->getAsObject();
  ASSERT_NE(Fields, nullptr);

  const llvm::json::Array *Outliers = Fields->getArray("outliers");
  ASSERT_NE(Outliers, nullptr);
  // The rarest first, and the value carrying the run does not appear.
  EXPECT_EQ(ToString(*Outliers),
            R"([{"count":1,"first_hit":22,"first_seq":22,"value":"once"},)"
            R"({"count":2,"first_hit":10,"first_seq":10,"value":"twice"}])");
}

TEST(AggregateTest, OutliersOfEqualCountAreOrderedByFirstSequence) {
  Aggregator Aggregate;
  uint64_t Seq = 0;
  for (; Seq < Aggregator::MinObservationsForOutliers; ++Seq)
    Aggregate.Record("loop", "kind", "common", Seq, Seq);

  // Both are seen once, and the one seen first sorts last as a value, so an
  // array following the value order rather than the sequence would show.
  Aggregate.Record("loop", "kind", "zulu", Seq, Seq);
  ++Seq;
  Aggregate.Record("loop", "kind", "alpha", Seq, Seq);
  ++Seq;

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Value *Entry = FindCapture(Summary, "loop", "kind");
  ASSERT_NE(Entry, nullptr);
  const llvm::json::Object *Fields = Entry->getAsObject();
  ASSERT_NE(Fields, nullptr);

  const llvm::json::Array *Outliers = Fields->getArray("outliers");
  ASSERT_NE(Outliers, nullptr);
  EXPECT_EQ(ToString(*Outliers),
            R"([{"count":1,"first_hit":20,"first_seq":20,"value":"zulu"},)"
            R"({"count":1,"first_hit":21,"first_seq":21,"value":"alpha"}])");
}

TEST(AggregateTest, OutliersAppearOnlyOnceThereAreEnoughObservations) {
  auto Build = [](uint64_t Hits) {
    Aggregator Aggregate;
    Aggregate.Record("loop", "kind", "rare", 0, 0);
    for (uint64_t Seq = 1; Seq < Hits; ++Seq)
      Aggregate.Record("loop", "kind", "common", Seq, Seq);
    return Aggregate;
  };

  const llvm::json::Value Below =
      Build(Aggregator::MinObservationsForOutliers - 1).Render();
  const llvm::json::Value *BelowEntry = FindCapture(Below, "loop", "kind");
  ASSERT_NE(BelowEntry, nullptr);
  const llvm::json::Object *BelowFields = BelowEntry->getAsObject();
  ASSERT_NE(BelowFields, nullptr);
  // The value is still counted; what is withheld is calling it remarkable.
  EXPECT_EQ(BelowFields->get("outliers"), nullptr);
  const llvm::json::Object *BelowValues = BelowFields->getObject("values");
  ASSERT_NE(BelowValues, nullptr);
  EXPECT_EQ(BelowValues->getInteger("rare"), std::optional<int64_t>(1));

  const llvm::json::Value At =
      Build(Aggregator::MinObservationsForOutliers).Render();
  const llvm::json::Value *AtEntry = FindCapture(At, "loop", "kind");
  ASSERT_NE(AtEntry, nullptr);
  const llvm::json::Object *AtFields = AtEntry->getAsObject();
  ASSERT_NE(AtFields, nullptr);
  const llvm::json::Array *Outliers = AtFields->getArray("outliers");
  ASSERT_NE(Outliers, nullptr);
  EXPECT_EQ(ToString(*Outliers),
            R"([{"count":1,"first_hit":0,"first_seq":0,"value":"rare"}])");
}

TEST(AggregateTest, LabelsAndCapturesAccumulateIndependently) {
  Aggregator Aggregate;
  Aggregate.Record("enter", "n", "1", 0, 0);
  Aggregate.Record("exit", "n", "2", 1, 1);
  Aggregate.Record("enter", "m", "3", 2, 2);
  Aggregate.Record("enter", "n", "1", 3, 3);

  EXPECT_EQ(ToString(Aggregate.Render()),
            R"({"enter":{"m":"3 x1","n":"1 x2"},"exit":{"n":"2 x1"}})");
}

TEST(AggregateTest, RenderIsStableAndSortsKeys) {
  Aggregator Aggregate;
  // Recorded in the reverse of the order they have to render in, so a
  // rendering that followed the order of arrival would show.
  Aggregate.Record("zeta", "v", "3", 0, 0);
  Aggregate.Record("alpha", "v", "2", 1, 1);

  const std::string Text = ToString(Aggregate.Render());
  EXPECT_EQ(Text, R"({"alpha":{"v":"2 x1"},"zeta":{"v":"3 x1"}})");
  EXPECT_EQ(Text, ToString(Aggregate.Render()));
}

TEST(AggregateTest, ValueCountsRenderInValueOrder) {
  Aggregator Aggregate;
  Aggregate.Record("loop", "v", "b", 0, 0);
  Aggregate.Record("loop", "v", "a", 1, 1);
  Aggregate.Record("loop", "v", "b", 2, 2);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Value *Entry = FindCapture(Summary, "loop", "v");
  ASSERT_NE(Entry, nullptr);
  const llvm::json::Object *Fields = Entry->getAsObject();
  ASSERT_NE(Fields, nullptr);
  const llvm::json::Object *Values = Fields->getObject("values");
  ASSERT_NE(Values, nullptr);
  EXPECT_EQ(ToString(*Values), R"({"a":1,"b":2})");
}

TEST(DetectCycleTest, RepeatedBlockIsReportedWithItsPeriodAndCount) {
  const std::vector<std::string> Tail = Repeat({"parse", "emit", "advance"}, 7);

  const std::optional<CycleReport> Cycle = DetectCycle(Tail);
  ASSERT_TRUE(Cycle.has_value());
  EXPECT_EQ(Cycle->Period, 3u);
  EXPECT_EQ(Cycle->Repeats, 7u);
  EXPECT_EQ(Cycle->Sequence,
            std::vector<std::string>({"parse", "emit", "advance"}));
}

TEST(DetectCycleTest, SingleLocationOverAndOverIsAPeriodOneCycle) {
  const std::vector<std::string> Tail = Repeat({"spin"}, 10);

  const std::optional<CycleReport> Cycle = DetectCycle(Tail);
  ASSERT_TRUE(Cycle.has_value());
  EXPECT_EQ(Cycle->Period, 1u);
  EXPECT_EQ(Cycle->Repeats, 10u);
  EXPECT_EQ(Cycle->Sequence, std::vector<std::string>({"spin"}));
}

TEST(DetectCycleTest, ShortestPeriodWins) {
  // Repeating "a b" six times over is also "a b a b" three times over, and the
  // shorter block is the one that describes the loop.
  const std::vector<std::string> Tail = Repeat({"a", "b"}, 6);

  const std::optional<CycleReport> Cycle = DetectCycle(Tail);
  ASSERT_TRUE(Cycle.has_value());
  EXPECT_EQ(Cycle->Period, 2u);
  EXPECT_EQ(Cycle->Repeats, 6u);
  EXPECT_EQ(Cycle->Sequence, std::vector<std::string>({"a", "b"}));
}

TEST(DetectCycleTest, CycleIsFoundAtAnyPhaseOfTheLoop) {
  // The tail begins wherever the stream was cut, so the block reported is the
  // loop rotated to end at the last entry.
  std::vector<std::string> Tail = Repeat({"a", "b", "c"}, 3);
  Tail.push_back("a");
  Tail.push_back("b");

  const std::optional<CycleReport> Cycle = DetectCycle(Tail);
  ASSERT_TRUE(Cycle.has_value());
  EXPECT_EQ(Cycle->Period, 3u);
  EXPECT_EQ(Cycle->Repeats, 3u);
  EXPECT_EQ(Cycle->Sequence, std::vector<std::string>({"c", "a", "b"}));
}

TEST(DetectCycleTest, UnrepeatedTailIsNotACycle) {
  const std::vector<std::string> Tail = {"a", "b", "c", "d",
                                         "e", "f", "g", "h"};
  EXPECT_FALSE(DetectCycle(Tail).has_value());
}

TEST(DetectCycleTest, TwoRepetitionsAreNotACycle) {
  const std::vector<std::string> Tail = Repeat({"x", "y", "z"}, 2);
  EXPECT_FALSE(DetectCycle(Tail).has_value());
}

TEST(DetectCycleTest, RepetitionThatDoesNotReachTheEndIsNotACycle) {
  // A loop the program has already left is not where it is now.
  std::vector<std::string> Tail = Repeat({"a", "b", "c"}, 4);
  Tail.push_back("cleanup");

  EXPECT_FALSE(DetectCycle(Tail).has_value());
}

TEST(DetectCycleTest, TailShorterThanThreePeriodsIsNotACycle) {
  const std::vector<std::string> Empty;
  EXPECT_FALSE(DetectCycle(Empty).has_value());
  EXPECT_FALSE(DetectCycle(Repeat({"a"}, 2)).has_value());
  // Two and a half repetitions of "a b" are not three of anything.
  EXPECT_FALSE(DetectCycle({"a", "b", "a", "b", "a"}).has_value());
}

TEST(DetectCycleTest, BlockLongerThanTheMaximumPeriodIsNotACycle) {
  std::vector<std::string> Block;
  for (unsigned I = 0; I <= MaxCyclePeriod; ++I)
    Block.push_back("loc" + std::to_string(I));

  std::vector<std::string> Tail;
  for (unsigned I = 0; I < 3; ++I)
    Tail.insert(Tail.end(), Block.begin(), Block.end());

  EXPECT_FALSE(DetectCycle(Tail).has_value());
}

TEST(AggregateTest, HistogramIsBoundedAndSaysWhatItDropped) {
  // A capture rendering something unique per hit would otherwise put one key
  // per hit into the response.
  Aggregator Agg;
  const uint64_t Hits = Aggregator::MaxHistogramValues + 20;
  for (uint64_t I = 0; I < Hits; ++I)
    Agg.Record("loop", "addr", "0x" + std::to_string(0x1000 + I), I, I);
  // One value made frequent, so the bound keeps something meaningful.
  for (uint64_t I = 0; I < 50; ++I)
    Agg.Record("loop", "addr", "0xbeef", Hits + I, Hits + I);

  llvm::json::Value Rendered = Agg.Render();
  const llvm::json::Object *Root = Rendered.getAsObject();
  ASSERT_NE(Root, nullptr);
  const llvm::json::Object *Loop = Root->getObject("loop");
  ASSERT_NE(Loop, nullptr);
  const llvm::json::Object *Addr = Loop->getObject("addr");
  ASSERT_NE(Addr, nullptr);
  const llvm::json::Object *Values = Addr->getObject("values");
  ASSERT_NE(Values, nullptr);

  // The bound plus the elision note, and the frequent value survived.
  EXPECT_EQ(Values->size(), Aggregator::MaxHistogramValues + 1);
  EXPECT_NE(Values->get("_elided"), nullptr);
  ASSERT_NE(Values->get("0xbeef"), nullptr);
  EXPECT_EQ(Values->getInteger("0xbeef"), std::optional<int64_t>(50));

  // distinct still counts everything, so the bound cannot be mistaken for the
  // real cardinality.
  EXPECT_EQ(Addr->getInteger("distinct"),
            std::optional<int64_t>(Hits + 1));
}

TEST(AggregateTest, OutlierReportsBothTheHitAndTheSequence) {
  // only_hit counts one observation's hits while the artifact is numbered by
  // the whole event stream, so the two differ as soon as a plan holds more than
  // one observation. Reporting only the sequence would send a caller acting on
  // an outlier to the wrong hit.
  Aggregator Agg;
  for (uint64_t Hit = 1; Hit <= 30; ++Hit) {
    const uint64_t Seq = 100 + Hit; // as if another observation ran first
    Agg.Record("values", "value", Hit == 7 ? "99" : "7", Seq, Hit);
  }

  llvm::json::Value Rendered = Agg.Render();
  const llvm::json::Array *Outliers = Rendered.getAsObject()
                                          ->getObject("values")
                                          ->getObject("value")
                                          ->getArray("outliers");
  ASSERT_NE(Outliers, nullptr);
  ASSERT_EQ(Outliers->size(), 1u);
  const llvm::json::Object *Outlier = (*Outliers)[0].getAsObject();
  EXPECT_EQ(Outlier->getString("value"), std::optional<llvm::StringRef>("99"));
  EXPECT_EQ(Outlier->getInteger("first_hit"), std::optional<int64_t>(7));
  EXPECT_EQ(Outlier->getInteger("first_seq"), std::optional<int64_t>(107));
}
