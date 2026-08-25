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
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <cstddef>
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

/// The rendered fields for one capture, or null when the capture collapsed to a
/// string because it never varied.
const llvm::json::Object *FindFields(const llvm::json::Value &Summary,
                                     llvm::StringRef Label,
                                     llvm::StringRef Capture) {
  const llvm::json::Value *Entry = FindCapture(Summary, Label, Capture);
  if (!Entry)
    return nullptr;
  return Entry->getAsObject();
}

/// A distinct value for hit \p Index, zero padded so that its order as a string
/// is its numeric order. A rendering that keeps values by frequency breaks ties
/// on the value, so an assertion about which values survive a bound only means
/// something when the two orders agree.
std::string ValueName(size_t Index) {
  const std::string Text = std::to_string(Index);
  return "v" + std::string(Text.size() < 3 ? 3 - Text.size() : 0, '0') + Text;
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

/// One tail entry as the engine records it: the location a hit came from, and
/// what that hit's captures rendered as, joined the way \ref DetectCycle expects.
std::string Entry(llvm::StringRef Location, llvm::StringRef Values) {
  std::string Out = Location.str();
  Out += CycleEntryValueSeparator;
  Out += Values;
  return Out;
}

} // namespace

TEST(AggregateTest, NothingRecordedRendersAnEmptyObject) {
  Aggregator Aggregate;
  EXPECT_EQ(ToString(Aggregate.Render()), "{}");
}

TEST(AggregateTest, SingleValuedCaptureCollapsesToAString) {
  Aggregator Aggregate;
  for (uint64_t Seq = 0; Seq < 4012; ++Seq)
    Aggregate.Record("loop", "done", "false", /*IsDocument=*/false, Seq, Seq);

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
  Aggregate.Record("loop", "n", "7", /*IsDocument=*/false, 0, 0);
  Aggregate.Record("loop", "n", "7", /*IsDocument=*/false, 100, 100);
  Aggregate.Record("loop", "n", "7", /*IsDocument=*/false, 250, 250);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Value *Entry = FindCapture(Summary, "loop", "n");
  ASSERT_NE(Entry, nullptr);
  ASSERT_TRUE(Entry->getAsString().has_value());
  EXPECT_EQ(Entry->getAsString()->str(), "7 x3");
}

TEST(AggregateTest, MultiValuedCaptureRendersAnObject) {
  Aggregator Aggregate;
  Aggregate.Record("loop", "state", "idle", /*IsDocument=*/false, 0, 0);
  Aggregate.Record("loop", "state", "idle", /*IsDocument=*/false, 1, 1);
  Aggregate.Record("loop", "state", "busy", /*IsDocument=*/false, 2, 2);
  Aggregate.Record("loop", "state", "idle", /*IsDocument=*/false, 3, 3);

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
  EXPECT_EQ(ToString(*Transitions),
            R"([{"count":1,"first_seq":2,"from":"idle","to":"busy"},)"
            R"({"count":1,"first_seq":3,"from":"busy","to":"idle"}])");

  // Four observations are too few for a rare value to mean anything.
  EXPECT_EQ(Fields->get("outliers"), nullptr);
}

TEST(AggregateTest, TransitionsCoverOnlyChangesAndStayInSequenceOrder) {
  Aggregator Aggregate;
  const llvm::StringRef Values[] = {"a", "a", "b", "b", "b", "c", "a"};
  uint64_t Seq = 0;
  for (llvm::StringRef Value : Values) {
    Aggregate.Record("loop", "phase", Value, /*IsDocument=*/false, Seq, Seq);
    ++Seq;
  }

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Value *Entry = FindCapture(Summary, "loop", "phase");
  ASSERT_NE(Entry, nullptr);
  const llvm::json::Object *Fields = Entry->getAsObject();
  ASSERT_NE(Fields, nullptr);

  const llvm::json::Array *Transitions = Fields->getArray("transitions");
  ASSERT_NE(Transitions, nullptr);
  EXPECT_EQ(ToString(*Transitions),
            R"([{"count":1,"first_seq":2,"from":"a","to":"b"},)"
            R"({"count":1,"first_seq":5,"from":"b","to":"c"},)"
            R"({"count":1,"first_seq":6,"from":"c","to":"a"}])");
}

TEST(AggregateTest, OutliersReportValuesSeenOnceOrTwiceAmongMany) {
  Aggregator Aggregate;
  uint64_t Seq = 0;
  auto RecordCommon = [&](unsigned Times) {
    for (unsigned I = 0; I < Times; ++I) {
      Aggregate.Record("loop", "kind", "common", /*IsDocument=*/false, Seq, Seq);
      ++Seq;
    }
  };

  RecordCommon(10);
  Aggregate.Record("loop", "kind", "twice", /*IsDocument=*/false, Seq, Seq);
  ++Seq;
  RecordCommon(5);
  Aggregate.Record("loop", "kind", "twice", /*IsDocument=*/false, Seq, Seq);
  ++Seq;
  RecordCommon(5);
  Aggregate.Record("loop", "kind", "once", /*IsDocument=*/false, Seq, Seq);
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
    Aggregate.Record("loop", "kind", "common", /*IsDocument=*/false, Seq, Seq);

  // Both are seen once, and the one seen first sorts last as a value, so an
  // array following the value order rather than the sequence would show.
  Aggregate.Record("loop", "kind", "zulu", /*IsDocument=*/false, Seq, Seq);
  ++Seq;
  Aggregate.Record("loop", "kind", "alpha", /*IsDocument=*/false, Seq, Seq);
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
    Aggregate.Record("loop", "kind", "rare", /*IsDocument=*/false, 0, 0);
    for (uint64_t Seq = 1; Seq < Hits; ++Seq)
      Aggregate.Record("loop", "kind", "common", /*IsDocument=*/false, Seq, Seq);
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
  Aggregate.Record("enter", "n", "1", /*IsDocument=*/false, 0, 0);
  Aggregate.Record("exit", "n", "2", /*IsDocument=*/false, 1, 1);
  Aggregate.Record("enter", "m", "3", /*IsDocument=*/false, 2, 2);
  Aggregate.Record("enter", "n", "1", /*IsDocument=*/false, 3, 3);

  EXPECT_EQ(ToString(Aggregate.Render()),
            R"({"enter":{"m":"3 x1","n":"1 x2"},"exit":{"n":"2 x1"}})");
}

TEST(AggregateTest, RenderIsStableAndSortsKeys) {
  Aggregator Aggregate;
  // Recorded in the reverse of the order they have to render in, so a
  // rendering that followed the order of arrival would show.
  Aggregate.Record("zeta", "v", "3", /*IsDocument=*/false, 0, 0);
  Aggregate.Record("alpha", "v", "2", /*IsDocument=*/false, 1, 1);

  const std::string Text = ToString(Aggregate.Render());
  EXPECT_EQ(Text, R"({"alpha":{"v":"2 x1"},"zeta":{"v":"3 x1"}})");
  EXPECT_EQ(Text, ToString(Aggregate.Render()));
}

TEST(AggregateTest, ValueCountsRenderInValueOrder) {
  Aggregator Aggregate;
  Aggregate.Record("loop", "v", "b", /*IsDocument=*/false, 0, 0);
  Aggregate.Record("loop", "v", "a", /*IsDocument=*/false, 1, 1);
  Aggregate.Record("loop", "v", "b", /*IsDocument=*/false, 2, 2);

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

TEST(DetectCycleTest, OneLocationOverAndOverIsNotACycle) {
  // The shape every timed-out run of a single observation ends in. Reported as a
  // cycle it read "period 1, repeats 256, this label" for a counted loop making
  // perfect progress as readily as for a hang -- and the 256 was the length of the
  // tail buffer rather than anything about the program.
  EXPECT_FALSE(DetectCycle(Repeat({"spin"}, 10)).has_value());
}

TEST(DetectCycleTest, ABlockOfIdenticalEntriesIsNotACycleAtAnyPeriod) {
  // The same claim as above reached by counting the repetitions in twos and in
  // threes. A block has to hold two distinct entries before it describes a
  // traversal rather than a place.
  EXPECT_FALSE(DetectCycle(Repeat({"spin", "spin"}, 6)).has_value());
  EXPECT_FALSE(DetectCycle(Repeat({"spin", "spin", "spin"}, 5)).has_value());
}

TEST(DetectCycleTest, TwoLocationsAlternatingIsACycle) {
  // The shortest block that says the program is traversing something.
  const std::optional<CycleReport> Cycle =
      DetectCycle(Repeat({"lock", "wait"}, 5));
  ASSERT_TRUE(Cycle.has_value());
  EXPECT_EQ(Cycle->Period, 2u);
  EXPECT_EQ(Cycle->Repeats, 5u);
  EXPECT_EQ(Cycle->Sequence, std::vector<std::string>({"lock", "wait"}));
}

TEST(DetectCycleTest, ABlockThatRepeatsOneEntryWithinItIsStillACycle) {
  // The block the corpus's one genuine cycle had: four entries, three distinct,
  // one of them twice in a row. Two distinct entries is the bar, not four.
  const std::optional<CycleReport> Cycle = DetectCycle(
      Repeat({"sinkcast_clone", "sinkcast_erase", "phiconvert", "phiconvert"},
             4));
  ASSERT_TRUE(Cycle.has_value());
  EXPECT_EQ(Cycle->Period, 4u);
  EXPECT_EQ(Cycle->Repeats, 4u);
  EXPECT_EQ(Cycle->Sequence,
            std::vector<std::string>({"sinkcast_clone", "sinkcast_erase",
                                      "phiconvert", "phiconvert"}));
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
    Agg.Record("loop", "addr", "0x" + std::to_string(0x1000 + I), /*IsDocument=*/false, I, I);
  // One value made frequent, so the bound keeps something meaningful.
  for (uint64_t I = 0; I < 50; ++I)
    Agg.Record("loop", "addr", "0xbeef", /*IsDocument=*/false, Hits + I, Hits + I);

  llvm::json::Value Rendered = Agg.Render();
  const llvm::json::Object *Root = Rendered.getAsObject();
  ASSERT_NE(Root, nullptr);
  const llvm::json::Object *Loop = Root->getObject("loop");
  ASSERT_NE(Loop, nullptr);
  const llvm::json::Object *Addr = Loop->getObject("addr");
  ASSERT_NE(Addr, nullptr);
  const llvm::json::Object *Values = Addr->getObject("values");
  ASSERT_NE(Values, nullptr);

  // The histogram holds only values now: the count of what was dropped lives
  // beside it, so it cannot collide with a value of that name.
  EXPECT_EQ(Values->size(), Aggregator::MaxHistogramValues);
  EXPECT_EQ(Values->get("_elided"), nullptr);
  EXPECT_GT(Addr->getInteger("values_elided").value_or(0), 0);
  ASSERT_NE(Values->get("0xbeef"), nullptr);
  EXPECT_EQ(Values->getInteger("0xbeef"), std::optional<int64_t>(50));

  // distinct still counts everything, so the bound cannot be mistaken for the
  // real cardinality.
  EXPECT_EQ(Addr->getInteger("distinct"), std::optional<int64_t>(Hits + 1));
}

TEST(AggregateTest, OutlierReportsBothTheHitAndTheSequence) {
  // only_hit counts one observation's hits while the artifact is numbered by
  // the whole event stream, so the two differ as soon as a plan holds more than
  // one observation. Reporting only the sequence would send a caller acting on
  // an outlier to the wrong hit.
  Aggregator Agg;
  for (uint64_t Hit = 1; Hit <= 30; ++Hit) {
    const uint64_t Seq = 100 + Hit; // as if another observation ran first
    Agg.Record("values", "value", Hit == 7 ? "99" : "7", /*IsDocument=*/false, Seq, Hit);
  }

  llvm::json::Value Rendered = Agg.Render();
  const llvm::json::Array *Outliers =
      Rendered.getAsObject()->getObject("values")->getObject("value")->getArray(
          "outliers");
  ASSERT_NE(Outliers, nullptr);
  ASSERT_EQ(Outliers->size(), 1u);
  const llvm::json::Object *Outlier = (*Outliers)[0].getAsObject();
  EXPECT_EQ(Outlier->getString("value"), std::optional<llvm::StringRef>("99"));
  EXPECT_EQ(Outlier->getInteger("first_hit"), std::optional<int64_t>(7));
  EXPECT_EQ(Outlier->getInteger("first_seq"), std::optional<int64_t>(107));
}

TEST(AggregateTest, OneDistinctValueCollapsesAndTwoDoNot) {
  Aggregator One;
  for (uint64_t Seq = 0; Seq < 5; ++Seq)
    One.Record("loop", "v", "same", /*IsDocument=*/false, Seq, Seq);

  const llvm::json::Value OneRendered = One.Render();
  const llvm::json::Value *OneEntry = FindCapture(OneRendered, "loop", "v");
  ASSERT_NE(OneEntry, nullptr);
  EXPECT_EQ(ToString(*OneEntry), R"("same x5")");

  // The same hits with one of them differing. A second distinct value is the
  // whole of what separates the two shapes.
  Aggregator Two;
  for (uint64_t Seq = 0; Seq < 4; ++Seq)
    Two.Record("loop", "v", "same", /*IsDocument=*/false, Seq, Seq);
  Two.Record("loop", "v", "other", /*IsDocument=*/false, 4, 4);

  const llvm::json::Value TwoRendered = Two.Render();
  EXPECT_EQ(ToString(TwoRendered),
            R"({"loop":{"v":{"distinct":2,"transitions":)"
            R"([{"count":1,"first_seq":4,"from":"same","to":"other"}],)"
            R"("values":{"other":1,"same":4}}}})");
}

TEST(AggregateTest, CollapsedCaptureCarriesNoneOfTheSummaryFields) {
  Aggregator Aggregate;
  const uint64_t Hits = Aggregator::MinObservationsForOutliers + 5;
  for (uint64_t Seq = 0; Seq < Hits; ++Seq)
    Aggregate.Record("loop", "v", "steady", /*IsDocument=*/false, Seq, Seq);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Value *Entry = FindCapture(Summary, "loop", "v");
  ASSERT_NE(Entry, nullptr);
  // Hits enough for an outlier section, and still a string: the collapse
  // depends on the number of distinct values alone.
  EXPECT_EQ(Entry->getAsObject(), nullptr);
  ASSERT_TRUE(Entry->getAsString().has_value());
  EXPECT_EQ(Entry->getAsString()->str(), "steady x25");
}

TEST(AggregateTest, VariedCaptureAlwaysCarriesDistinctValuesAndTransitions) {
  // Two distinct values cannot both be seen without a change between them, so
  // the object form never renders an empty transitions array.
  Aggregator Aggregate;
  Aggregate.Record("loop", "v", "a", /*IsDocument=*/false, 0, 0);
  Aggregate.Record("loop", "v", "b", /*IsDocument=*/false, 1, 1);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "v");
  ASSERT_NE(Fields, nullptr);
  EXPECT_EQ(Fields->getInteger("distinct"), std::optional<int64_t>(2));
  EXPECT_NE(Fields->getObject("values"), nullptr);
  const llvm::json::Array *Transitions = Fields->getArray("transitions");
  ASSERT_NE(Transitions, nullptr);
  EXPECT_EQ(Transitions->size(), 1u);
  EXPECT_EQ(Fields->get("outliers"), nullptr);
}

TEST(AggregateTest, OutliersAreAbsentWhenNothingIsRareEnough) {
  // The observation threshold is necessary and not sufficient: a run of two
  // values in equal measure has nothing remarkable to name.
  Aggregator Aggregate;
  const uint64_t Hits = Aggregator::MinObservationsForOutliers + 10;
  for (uint64_t Seq = 0; Seq < Hits; ++Seq)
    Aggregate.Record("loop", "v", Seq % 2 == 0 ? "even" : "odd", /*IsDocument=*/false, Seq, Seq);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "v");
  ASSERT_NE(Fields, nullptr);
  const llvm::json::Object *Values = Fields->getObject("values");
  ASSERT_NE(Values, nullptr);
  EXPECT_EQ(Values->getInteger("even"), std::optional<int64_t>(15));
  EXPECT_EQ(Values->getInteger("odd"), std::optional<int64_t>(15));
  EXPECT_EQ(Fields->get("outliers"), nullptr);
}

TEST(AggregateTest, OutliersRemainAboveTheObservationThreshold) {
  Aggregator Aggregate;
  Aggregate.Record("loop", "kind", "rare", /*IsDocument=*/false, 0, 0);
  const uint64_t Hits = Aggregator::MinObservationsForOutliers + 10;
  for (uint64_t Seq = 1; Seq < Hits; ++Seq)
    Aggregate.Record("loop", "kind", "common", /*IsDocument=*/false, Seq, Seq);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "kind");
  ASSERT_NE(Fields, nullptr);
  const llvm::json::Array *Outliers = Fields->getArray("outliers");
  ASSERT_NE(Outliers, nullptr);
  EXPECT_EQ(ToString(*Outliers),
            R"([{"count":1,"first_hit":0,"first_seq":0,"value":"rare"}])");
}

TEST(AggregateTest, AValueSeenThreeTimesIsNotRare) {
  Aggregator Aggregate;
  uint64_t Seq = 0;
  auto Record = [&](llvm::StringRef Value, unsigned Times) {
    for (unsigned I = 0; I < Times; ++I) {
      Aggregate.Record("loop", "kind", Value, /*IsDocument=*/false, Seq, Seq);
      ++Seq;
    }
  };

  Record("once", 1);
  Record("twice", 2);
  Record("thrice", 3);
  Record("common", 20);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "kind");
  ASSERT_NE(Fields, nullptr);

  // The third sighting is what stops a value being worth naming, so the bound
  // is on the count and not on a share of the hits.
  const llvm::json::Array *Outliers = Fields->getArray("outliers");
  ASSERT_NE(Outliers, nullptr);
  EXPECT_EQ(ToString(*Outliers),
            R"([{"count":1,"first_hit":0,"first_seq":0,"value":"once"},)"
            R"({"count":2,"first_hit":1,"first_seq":1,"value":"twice"}])");

  const llvm::json::Object *Values = Fields->getObject("values");
  ASSERT_NE(Values, nullptr);
  EXPECT_EQ(Values->getInteger("thrice"), std::optional<int64_t>(3));
}

TEST(AggregateTest, OutliersAreOrderedByCountThenByFirstSequence) {
  Aggregator Aggregate;
  uint64_t Seq = 0;
  auto Record = [&](llvm::StringRef Value) {
    Aggregate.Record("loop", "kind", Value, /*IsDocument=*/false, Seq, Seq);
    ++Seq;
  };

  // Neither the value order nor the arrival order is the order the array has to
  // have, so a rendering that followed either of them shows.
  Record("d-twice");
  Record("b-once");
  Record("c-twice");
  Record("a-once");
  Record("d-twice");
  Record("c-twice");
  for (unsigned I = 0; I < Aggregator::MinObservationsForOutliers; ++I)
    Record("common");

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "kind");
  ASSERT_NE(Fields, nullptr);
  const llvm::json::Array *Outliers = Fields->getArray("outliers");
  ASSERT_NE(Outliers, nullptr);
  // Rarity decides first, which puts a value seen twice early in the run after
  // one seen once late in it.
  EXPECT_EQ(ToString(*Outliers),
            R"([{"count":1,"first_hit":1,"first_seq":1,"value":"b-once"},)"
            R"({"count":1,"first_hit":3,"first_seq":3,"value":"a-once"},)"
            R"({"count":2,"first_hit":0,"first_seq":0,"value":"d-twice"},)"
            R"({"count":2,"first_hit":2,"first_seq":2,"value":"c-twice"}])");
}

TEST(AggregateTest, TheHitAndTheSequenceAreReportedIndependently) {
  // The two numbers answer different questions: only_hit takes the hit, while a
  // transition and an outlier are matched against a line of the artifact by the
  // sequence. Reporting either in place of the other sends a caller to the
  // wrong event.
  Aggregator Aggregate;
  const uint64_t Hits = Aggregator::MinObservationsForOutliers + 5;
  for (uint64_t Hit = 0; Hit < Hits; ++Hit)
    Aggregate.Record("loop", "v", Hit == 3 ? "spike" : "flat",
                     /*IsDocument=*/false, 1000 + Hit * 7, Hit);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "v");
  ASSERT_NE(Fields, nullptr);

  const llvm::json::Array *Transitions = Fields->getArray("transitions");
  ASSERT_NE(Transitions, nullptr);
  EXPECT_EQ(ToString(*Transitions),
            R"([{"count":1,"first_seq":1021,"from":"flat","to":"spike"},)"
            R"({"count":1,"first_seq":1028,"from":"spike","to":"flat"}])");

  const llvm::json::Array *Outliers = Fields->getArray("outliers");
  ASSERT_NE(Outliers, nullptr);
  EXPECT_EQ(ToString(*Outliers),
            R"([{"count":1,"first_hit":3,"first_seq":1021,"value":"spike"}])");
}

TEST(AggregateTest, HistogramKeepsEveryValueUpToTheBound) {
  auto Build = [](size_t Distinct) {
    Aggregator Aggregate;
    for (size_t I = 0; I < Distinct; ++I)
      Aggregate.Record("loop", "v", ValueName(I), /*IsDocument=*/false, I, I);
    return Aggregate;
  };

  for (size_t Distinct :
       {Aggregator::MaxHistogramValues - 1, Aggregator::MaxHistogramValues}) {
    Aggregator Aggregate = Build(Distinct);
    const llvm::json::Value Summary = Aggregate.Render();
    const llvm::json::Object *Fields = FindFields(Summary, "loop", "v");
    ASSERT_NE(Fields, nullptr) << Distinct;
    const llvm::json::Object *Values = Fields->getObject("values");
    ASSERT_NE(Values, nullptr) << Distinct;
    EXPECT_EQ(Values->size(), Distinct);
    EXPECT_EQ(Fields->get("values_elided"), nullptr) << Distinct;
    EXPECT_EQ(Fields->getInteger("distinct"),
              std::optional<int64_t>(static_cast<int64_t>(Distinct)));
  }
}

TEST(AggregateTest, HistogramPastTheBoundDropsTheLeastFrequentAndSaysHowMany) {
  const size_t Distinct = Aggregator::MaxHistogramValues + 1;
  Aggregator Aggregate;
  uint64_t Seq = 0;
  // Every value but the last one made frequent, so which of them the bound
  // drops is decided by frequency rather than by a tie.
  for (size_t I = 0; I + 1 < Distinct; ++I)
    for (unsigned Repeat = 0; Repeat < 5; ++Repeat, ++Seq)
      Aggregate.Record("loop", "v", ValueName(I), /*IsDocument=*/false, Seq, Seq);
  const uint64_t RareSeq = Seq;
  Aggregate.Record("loop", "v", ValueName(Distinct - 1), /*IsDocument=*/false, RareSeq, RareSeq);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "v");
  ASSERT_NE(Fields, nullptr);
  const llvm::json::Object *Values = Fields->getObject("values");
  ASSERT_NE(Values, nullptr);

  EXPECT_EQ(Values->size(), Aggregator::MaxHistogramValues);
  EXPECT_EQ(Values->get(ValueName(Distinct - 1)), nullptr);
  EXPECT_EQ(Values->getInteger(ValueName(0)), std::optional<int64_t>(5));
  // A count rather than prose, and outside the histogram so that a capture
  // rendering this key cannot have its own count overwritten by the note.
  EXPECT_EQ(Fields->getInteger("values_elided"), std::optional<int64_t>(1));

  // A shortened histogram would otherwise read as the whole cardinality.
  EXPECT_EQ(Fields->getInteger("distinct"),
            std::optional<int64_t>(static_cast<int64_t>(Distinct)));

  // Outliers are selected before the bound applies, which is what keeps the
  // rare value that is usually the answer from being the one dropped.
  const llvm::json::Array *Outliers = Fields->getArray("outliers");
  ASSERT_NE(Outliers, nullptr);
  ASSERT_EQ(Outliers->size(), 1u);
  const llvm::json::Object *Outlier = (*Outliers)[0].getAsObject();
  ASSERT_NE(Outlier, nullptr);
  EXPECT_EQ(Outlier->getString("value"),
            std::optional<llvm::StringRef>(ValueName(Distinct - 1)));
  EXPECT_EQ(Outlier->getInteger("first_seq"),
            std::optional<int64_t>(static_cast<int64_t>(RareSeq)));
}

TEST(AggregateTest, EquallyFrequentValuesPastTheBoundAreKeptInValueOrder) {
  // With nothing to choose between them by frequency, the value decides, so the
  // histogram does not depend on the order the values arrived in.
  const size_t Distinct = Aggregator::MaxHistogramValues + 1;
  Aggregator Aggregate;
  for (size_t I = Distinct; I > 0; --I)
    Aggregate.Record("loop", "v", ValueName(I - 1), /*IsDocument=*/false, Distinct - I, Distinct - I);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "v");
  ASSERT_NE(Fields, nullptr);
  const llvm::json::Object *Values = Fields->getObject("values");
  ASSERT_NE(Values, nullptr);
  EXPECT_NE(Values->get(ValueName(Aggregator::MaxHistogramValues - 1)),
            nullptr);
  EXPECT_EQ(Values->get(ValueName(Aggregator::MaxHistogramValues)), nullptr);
}

TEST(AggregateTest, InterleavedLabelsAndCapturesKeepSeparateHistories) {
  // Hits arrive interleaved, and a transition belongs to the history of one
  // capture of one label: chaining across them would invent changes.
  Aggregator Aggregate;
  Aggregate.Record("enter", "n", "1", /*IsDocument=*/false, 0, 0);
  Aggregate.Record("exit", "n", "1", /*IsDocument=*/false, 1, 0);
  Aggregate.Record("enter", "tag", "x", /*IsDocument=*/false, 2, 1);
  Aggregate.Record("exit", "tag", "y", /*IsDocument=*/false, 3, 1);
  Aggregate.Record("enter", "n", "2", /*IsDocument=*/false, 4, 2);
  Aggregate.Record("exit", "n", "1", /*IsDocument=*/false, 5, 2);
  Aggregate.Record("enter", "tag", "x", /*IsDocument=*/false, 6, 3);
  Aggregate.Record("exit", "tag", "z", /*IsDocument=*/false, 7, 3);
  Aggregate.Record("enter", "n", "1", /*IsDocument=*/false, 8, 4);

  EXPECT_EQ(ToString(Aggregate.Render()),
            R"({"enter":{"n":{"distinct":2,"transitions":)"
            R"([{"count":1,"first_seq":4,"from":"1","to":"2"},)"
            R"({"count":1,"first_seq":8,"from":"2","to":"1"}],)"
            R"("values":{"1":2,"2":1}},"tag":"x x2"},)"
            R"("exit":{"n":"1 x2","tag":{"distinct":2,"transitions":)"
            R"([{"count":1,"first_seq":7,"from":"y","to":"z"}],)"
            R"("values":{"y":1,"z":1}}}})");
}

TEST(AggregateTest, TheSameRecordingRendersByteIdenticallyTwice) {
  // Two responses over the same run are only diffable if nothing in the
  // rendering depends on how the maps were built or on how many times Render
  // has been called.
  auto Build = [] {
    Aggregator Aggregate;
    uint64_t Seq = 500;
    for (size_t I = 0; I < Aggregator::MaxHistogramValues + 3; ++I, ++Seq)
      Aggregate.Record("zeta", "addr", ValueName(I), /*IsDocument=*/false, Seq, Seq - 500);
    for (unsigned I = 0; I < Aggregator::MinObservationsForOutliers; ++I, ++Seq)
      Aggregate.Record("alpha", "state", I % 3 == 0 ? "busy" : "idle",
                       /*IsDocument=*/false, Seq, Seq - 500);
    Aggregate.Record("alpha", "state", "wedged", /*IsDocument=*/false, Seq, Seq - 500);
    return Aggregate;
  };

  Aggregator First = Build();
  const std::string Once = ToString(First.Render());
  EXPECT_EQ(Once, ToString(First.Render()));

  Aggregator Second = Build();
  EXPECT_EQ(Once, ToString(Second.Render()));

  // Every part of the rendering that has an order of its own is present, or the
  // comparison above proves little.
  EXPECT_NE(Once.find("_elided"), std::string::npos);
  EXPECT_NE(Once.find("outliers"), std::string::npos);
  EXPECT_NE(Once.find("transitions"), std::string::npos);
}

TEST(DetectCycleTest, PeriodAtTheMaximumRepeatedThreeTimesIsACycle) {
  std::vector<std::string> Block;
  for (unsigned I = 0; I < MaxCyclePeriod; ++I)
    Block.push_back("loc" + std::to_string(I));

  std::vector<std::string> Tail;
  for (unsigned I = 0; I < 3; ++I)
    Tail.insert(Tail.end(), Block.begin(), Block.end());

  const std::optional<CycleReport> Cycle = DetectCycle(Tail);
  ASSERT_TRUE(Cycle.has_value());
  EXPECT_EQ(Cycle->Period, MaxCyclePeriod);
  EXPECT_EQ(Cycle->Repeats, 3u);
  EXPECT_EQ(Cycle->Sequence, Block);
}

TEST(DetectCycleTest, PeriodAboveTheMaximumIsNotACycleHoweverOftenItRepeats) {
  std::vector<std::string> Block;
  for (unsigned I = 0; I <= MaxCyclePeriod; ++I)
    Block.push_back("loc" + std::to_string(I));

  std::vector<std::string> Tail;
  for (unsigned I = 0; I < 6; ++I)
    Tail.insert(Tail.end(), Block.begin(), Block.end());

  EXPECT_FALSE(DetectCycle(Tail).has_value());
}

TEST(DetectCycleTest, ThreeRepeatsIsACycleAndTwoIsNot) {
  const llvm::StringRef Block[] = {"lock", "work", "unlock", "yield"};

  const std::optional<CycleReport> Three = DetectCycle(Repeat(Block, 3));
  ASSERT_TRUE(Three.has_value());
  EXPECT_EQ(Three->Period, 4u);
  EXPECT_EQ(Three->Repeats, 3u);
  EXPECT_EQ(Three->Sequence,
            std::vector<std::string>({"lock", "work", "unlock", "yield"}));

  // One repetition short, and a block repeated twice is as much a coincidence
  // as a loop.
  EXPECT_FALSE(DetectCycle(Repeat(Block, 2)).has_value());
}

TEST(DetectCycleTest,
     ALoopCutMidIterationIsReportedRotatedToEndAtTheLastEntry) {
  // What the caller needs is where the program is now, so the block reported
  // ends at the final entry whatever phase the stream was cut in.
  std::vector<std::string> Tail =
      Repeat({"read", "decode", "execute", "retire"}, 5);
  Tail.push_back("read");
  Tail.push_back("decode");

  const std::optional<CycleReport> Cycle = DetectCycle(Tail);
  ASSERT_TRUE(Cycle.has_value());
  EXPECT_EQ(Cycle->Period, 4u);
  EXPECT_EQ(Cycle->Repeats, 5u);
  EXPECT_EQ(Cycle->Sequence,
            std::vector<std::string>({"execute", "retire", "read", "decode"}));
}

TEST(DetectCycleTest, ValuesJoinedToALocationDecideWhetherTheBlockRepeated) {
  // What separates a hang from a loop that is getting somewhere is not the places
  // it traverses -- those are the same either way -- but whether what it reads
  // there changes. A counted loop's values differ at every iteration, so no block
  // of them repeats however regular its locations are.
  std::vector<std::string> Tail;
  for (unsigned Iteration = 0; Iteration < 8; ++Iteration)
    for (llvm::StringRef Location : {"head", "body"})
      Tail.push_back(Entry(Location, std::to_string(Iteration)));

  EXPECT_FALSE(DetectCycle(Tail).has_value());
}

TEST(DetectCycleTest, ALoopReadingTheSameValuesIsACycleReportedAsLocations) {
  // The wedged case: the same places, and the same values at them, over and over.
  // What is reported is the locations alone, since the values are already in the
  // aggregate and in the artifact's tail and where the program is is what a caller
  // acts on.
  std::vector<std::string> Tail;
  for (unsigned Iteration = 0; Iteration < 4; ++Iteration) {
    Tail.push_back(Entry("acquire", "held=1"));
    Tail.push_back(Entry("retry", "held=1"));
  }

  const std::optional<CycleReport> Cycle = DetectCycle(Tail);
  ASSERT_TRUE(Cycle.has_value());
  EXPECT_EQ(Cycle->Period, 2u);
  EXPECT_EQ(Cycle->Repeats, 4u);
  EXPECT_EQ(Cycle->Sequence, std::vector<std::string>({"acquire", "retry"}));
}

TEST(DetectCycleTest, APlanWithNoCapturesIsJudgedOnLocationsAlone) {
  // The shortest plan for a hang carries no captures, so every entry has an empty
  // value half. That has to behave exactly as the labels alone did, because it is
  // the case the one genuine cycle in the corpus came from.
  std::vector<std::string> Tail;
  for (unsigned Iteration = 0; Iteration < 5; ++Iteration)
    for (llvm::StringRef Location : {"clone", "erase", "convert"})
      Tail.push_back(Entry(Location, ""));

  const std::optional<CycleReport> Cycle = DetectCycle(Tail);
  ASSERT_TRUE(Cycle.has_value());
  EXPECT_EQ(Cycle->Period, 3u);
  EXPECT_EQ(Cycle->Repeats, 5u);
  EXPECT_EQ(Cycle->Sequence,
            std::vector<std::string>({"clone", "erase", "convert"}));
}

TEST(AggregateTest, ANoteKeyIsNotConfusableWithAValueOfThatName) {
  // The count of dropped values used to live among the values, sharing their
  // namespace. A capture that rendered the note's own key had its count
  // overwritten and lost, with nothing left to show that it had been.
  Aggregator Aggregate;
  uint64_t Seq = 0;
  for (unsigned Repeat = 0; Repeat < 40; ++Repeat, ++Seq)
    Aggregate.Record("loop", "v", "_elided", /*IsDocument=*/false, Seq, Seq);
  for (size_t I = 0; I < Aggregator::MaxHistogramValues + 4; ++I, ++Seq)
    Aggregate.Record("loop", "v", ValueName(I), /*IsDocument=*/false, Seq, Seq);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "v");
  ASSERT_NE(Fields, nullptr);
  const llvm::json::Object *Values = Fields->getObject("values");
  ASSERT_NE(Values, nullptr);

  // The most frequent value by far, so the bound cannot be why it is missing.
  EXPECT_EQ(Values->getInteger("_elided"), std::optional<int64_t>(40));
  EXPECT_GT(Fields->getInteger("values_elided").value_or(0), 0);
}

TEST(AggregateTest, TransitionsCountTheDistinctChangesRatherThanListingThem) {
  // A value that cycles is the shape this exists for. Recording every change in
  // order and keeping the first few of them described the start of the run and
  // repeated one pair over and over; the distinct pairs with their counts
  // describe the whole of it, and stay the same size however long it runs.
  Aggregator Aggregate;
  const llvm::StringRef Cycle[] = {"legal", "custom"};
  for (uint64_t Seq = 0; Seq < 400; ++Seq)
    Aggregate.Record("loop", "state", Cycle[Seq % 2], /*IsDocument=*/false, Seq, Seq);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "state");
  ASSERT_NE(Fields, nullptr);
  const llvm::json::Array *Transitions = Fields->getArray("transitions");
  ASSERT_NE(Transitions, nullptr);
  EXPECT_EQ(
      ToString(*Transitions),
      R"([{"count":200,"first_seq":1,"from":"legal","to":"custom"},)"
      R"({"count":199,"first_seq":2,"from":"custom","to":"legal"}])");
  EXPECT_EQ(Fields->get("transitions_elided"), nullptr);
}

TEST(AggregateTest, TransitionsAreBoundedAndSayHowManyWereDropped) {
  // A capture that changes to a value it has never held before, on every hit,
  // has as many distinct changes as hits. Unbounded, that leaves the summary as
  // large as the stream it stands in for.
  Aggregator Aggregate;
  const size_t Changes = Aggregator::MaxTransitions + 25;
  uint64_t Seq = 0;
  // One pair traversed often, so the bound has something to prefer and the
  // entries kept are not decided by a tie.
  for (size_t I = 0; I < 30; ++I) {
    Aggregate.Record("loop", "v", "a", /*IsDocument=*/false, Seq, Seq);
    ++Seq;
    Aggregate.Record("loop", "v", "b", /*IsDocument=*/false, Seq, Seq);
    ++Seq;
  }
  for (size_t I = 0; I <= Changes; ++I) {
    Aggregate.Record("loop", "v", ValueName(I), /*IsDocument=*/false, Seq, Seq);
    ++Seq;
  }

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "v");
  ASSERT_NE(Fields, nullptr);
  const llvm::json::Array *Transitions = Fields->getArray("transitions");
  ASSERT_NE(Transitions, nullptr);

  EXPECT_EQ(Transitions->size(), Aggregator::MaxTransitions);
  EXPECT_GT(Fields->getInteger("transitions_elided").value_or(0), 0);

  // The most travelled edge survives whatever else does, since a bound that
  // dropped it would leave a summary of an oscillation with the oscillation
  // missing.
  const llvm::json::Object *First = (*Transitions)[0].getAsObject();
  ASSERT_NE(First, nullptr);
  EXPECT_EQ(First->getInteger("count"), std::optional<int64_t>(30));
}

TEST(AggregateTest, AnUndifferentiatedListIsShortenedToOneExample) {
  // Both lists answer "which of these dominate". Where every entry kept carries
  // the same count and most of the population is being dropped regardless, no
  // entry dominates and each one past the first repeats what the first said:
  // measured on a capture of node ids over 8000 hits, eight of two thousand
  // equally common values and eight of two thousand equally travelled edges,
  // beside counts saying how many thousands were dropped.
  Aggregator Aggregate;
  const size_t Distinct = Aggregator::MaxHistogramValues * 4;
  uint64_t Seq = 0;
  for (unsigned Round = 0; Round < 3; ++Round)
    for (size_t I = 0; I < Distinct; ++I, ++Seq)
      Aggregate.Record("loop", "id", ValueName(I), /*IsDocument=*/false, Seq, Seq);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "id");
  ASSERT_NE(Fields, nullptr);

  const llvm::json::Object *Values = Fields->getObject("values");
  ASSERT_NE(Values, nullptr);
  EXPECT_EQ(Values->size(), 1u);
  EXPECT_EQ(Fields->getInteger("values_elided"),
            std::optional<int64_t>(Distinct - 1));

  const llvm::json::Array *Transitions = Fields->getArray("transitions");
  ASSERT_NE(Transitions, nullptr);
  EXPECT_EQ(Transitions->size(), 1u);
  EXPECT_GT(Fields->getInteger("transitions_elided").value_or(0), 0);

  // What the shortening replaces is an enumeration, not a fact: the cardinality
  // and the count an example carries are both still there.
  EXPECT_EQ(Fields->getInteger("distinct"),
            std::optional<int64_t>(static_cast<int64_t>(Distinct)));
  EXPECT_EQ(Values->begin()->second.getAsInteger(), std::optional<int64_t>(3));
}

TEST(AggregateTest, AFewEquallyCommonValuesAreStillEnumerated) {
  // The shortening above applies only where most of the population is dropped
  // anyway. Nine equally common values are worth naming, and a summary that
  // showed one of them would be hiding eight names it had room for.
  const size_t Distinct = Aggregator::MaxHistogramValues + 1;
  Aggregator Aggregate;
  uint64_t Seq = 0;
  for (unsigned Round = 0; Round < 4; ++Round)
    for (size_t I = 0; I < Distinct; ++I, ++Seq)
      Aggregate.Record("loop", "opcode", ValueName(I), /*IsDocument=*/false, Seq, Seq);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "opcode");
  ASSERT_NE(Fields, nullptr);
  const llvm::json::Object *Values = Fields->getObject("values");
  ASSERT_NE(Values, nullptr);
  EXPECT_EQ(Values->size(), Aggregator::MaxHistogramValues);
  EXPECT_EQ(Fields->getInteger("values_elided"), std::optional<int64_t>(1));
}

TEST(AggregateTest, OutliersAreBoundedAndSayHowManyWereDropped) {
  // A capture rendering a pointer or an address is distinct at every hit, which
  // makes every one of its values rare. Unbounded, that puts one outlier per
  // hit into the response -- the same unboundedness the histogram cap exists to
  // prevent, reached through the door that is exempt from it.
  Aggregator Aggregate;
  const size_t Rare = Aggregator::MaxOutliers + 40;
  size_t Seq = 0;
  for (; Seq < Rare; ++Seq)
    Aggregate.Record("loop", "p", ValueName(Seq), /*IsDocument=*/false, Seq, Seq);
  // A common value the typical hit holds, so that being seen once is remarkable
  // rather than ordinary.
  for (size_t I = 0; I < Rare * 3; ++I, ++Seq)
    Aggregate.Record("loop", "p", "common", /*IsDocument=*/false, Seq, Seq);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "p");
  ASSERT_NE(Fields, nullptr);
  const llvm::json::Array *Outliers = Fields->getArray("outliers");
  ASSERT_NE(Outliers, nullptr);

  EXPECT_EQ(Outliers->size(), Aggregator::MaxOutliers);
  EXPECT_EQ(Fields->getInteger("outliers_elided"), std::optional<int64_t>(40));

  // Every value here is equally rare, so the tie-break decides, and it keeps
  // the earliest: the first sighting of a rare value is the whole reason to
  // report one.
  const llvm::json::Object *First = (*Outliers)[0].getAsObject();
  ASSERT_NE(First, nullptr);
  EXPECT_EQ(First->getString("value"),
            std::optional<llvm::StringRef>(ValueName(0)));

  // Nothing about the cardinality is hidden by the bound, so a shortened list
  // cannot be read as the whole of what was rare.
  EXPECT_EQ(Fields->getInteger("distinct"),
            std::optional<int64_t>(static_cast<int64_t>(Rare + 1)));
}

TEST(AggregateTest, NothingIsRareWhenTheTypicalHitIsItselfRare) {
  // Measured on a capture of an instruction pointer over 1200 hits: three
  // addresses held 135, 133 and 133 of them while four hundred held one or two.
  // Something was common by any share you like, and the answer was still eight
  // arbitrary addresses with 392 elided -- because two thirds of the hits were in
  // that tail, which is what makes the tail ordinary.
  Aggregator Aggregate;
  uint64_t Seq = 0;
  for (unsigned I = 0; I < 3; ++I)
    for (unsigned Repeat = 0; Repeat < 134; ++Repeat, ++Seq)
      Aggregate.Record("loop", "p", "hot" + std::to_string(I), /*IsDocument=*/false, Seq, Seq);
  for (unsigned I = 0; I < 400; ++I)
    for (unsigned Repeat = 0; Repeat < 2; ++Repeat, ++Seq)
      Aggregate.Record("loop", "p", ValueName(I), /*IsDocument=*/false, Seq, Seq);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "p");
  ASSERT_NE(Fields, nullptr);
  // Said rather than left absent, and said as a string where the array would be:
  // absent is what "nothing was rare" looks like, and these are opposite
  // readings of the field a caller is told to read first.
  EXPECT_EQ(Fields->getArray("outliers"), nullptr)
      << ToString(*Fields).substr(0, 300);
  EXPECT_THAT(Fields->getString("outliers").value_or("").str(),
              testing::HasSubstr("withheld"));
  EXPECT_EQ(Fields->getInteger("outliers_of"), std::optional<int64_t>(1202));
  // Withheld, not hidden: the histogram still shows what dominated.
  EXPECT_NE(Fields->getObject("values")->getInteger("hot0"), std::nullopt);
}

TEST(AggregateTest, OneRareValueAmongThousandsOfCommonOnesSurvives) {
  // The case the rule exists to keep, and the one every bound and gate here is
  // measured against: one vector type among four thousand integers.
  Aggregator Aggregate;
  uint64_t Seq = 0;
  for (; Seq < 4000; ++Seq)
    Aggregate.Record("loop", "ty", "i32", /*IsDocument=*/false, Seq, Seq);
  Aggregate.Record("loop", "ty", "v4i32", /*IsDocument=*/false, Seq, Seq);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "ty");
  ASSERT_NE(Fields, nullptr);
  const llvm::json::Array *Outliers = Fields->getArray("outliers");
  ASSERT_NE(Outliers, nullptr);
  EXPECT_EQ(ToString(*Outliers),
            R"([{"count":1,"first_hit":4000,"first_seq":4000,"value":"v4i32"}])");
}

TEST(AggregateTest, NothingIsRareWhenEveryValueIsDistinct) {
  // Measured on a capture of a compiler's node ids: 1808 hits, 1808 distinct
  // values, so every value qualified as seen-once and the list rendered 32 of
  // them plus a count of 1776 more. "Rare" is a claim about a value against the
  // ones that repeat, and there were none.
  Aggregator Aggregate;
  for (uint64_t Seq = 0; Seq < 200; ++Seq)
    Aggregate.Record("loop", "id", ValueName(Seq), /*IsDocument=*/false, Seq, Seq);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "id");
  ASSERT_NE(Fields, nullptr);
  EXPECT_EQ(Fields->getArray("outliers"), nullptr);
  EXPECT_EQ(Fields->get("outliers_elided"), nullptr);
  // The opposite reading of an absent `outliers` is that nothing was rare, and on
  // this run the two are opposite: everything was. `distinct: 30266` over 30,266
  // hits and `distinct: 0` rendered the same way, one label apart in the same
  // response.
  EXPECT_THAT(Fields->getString("outliers").value_or("").str(),
              testing::HasSubstr("withheld"));
  // The denominator of the claim being declined, which `values_elided` makes
  // unrecoverable from the histogram.
  EXPECT_EQ(Fields->getInteger("outliers_of"), std::optional<int64_t>(200));
  // Withheld, not hidden: the cardinality that makes the claim meaningless is
  // the first field of the summary.
  EXPECT_EQ(Fields->getInteger("distinct"), std::optional<int64_t>(200));
}

TEST(AggregateTest, NothingRareAndEverythingRareAreDifferentAnswers) {
  // Both of these came out of one run, one label apart, and read identically.
  // `kind` had no outliers because nothing was rare among the hits it saw; `seq`
  // had none because everything was, `distinct: 30266` over 30,266 hits. Those are
  // opposite readings of the field a caller is told is usually the answer.
  Aggregator Aggregate;
  uint64_t Seq = 0;
  for (uint64_t I = 0; I < 60; ++I, ++Seq)
    Aggregate.Record("scale", "kind", I % 2 == 0 ? "0" : "1",
                     /*IsDocument=*/false, Seq, Seq);
  for (uint64_t I = 0; I < 60; ++I, ++Seq)
    Aggregate.Record("scale", "seq", ValueName(I), /*IsDocument=*/false, Seq,
                     Seq);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Kind = FindFields(Summary, "scale", "kind");
  const llvm::json::Object *Uniq = FindFields(Summary, "scale", "seq");
  ASSERT_NE(Kind, nullptr);
  ASSERT_NE(Uniq, nullptr);

  // Nothing was rare: absent, which is now the only thing absent can mean.
  EXPECT_EQ(Kind->get("outliers"), nullptr) << ToString(*Kind);
  // Everything was: a string where the array would be, so a client that reads the
  // field as a list gets a type error at the point where it would otherwise have
  // concluded that nothing rare happened.
  EXPECT_NE(Uniq->getString("outliers"), std::nullopt) << ToString(*Uniq);
}

TEST(AggregateTest, AShortRunIsSilentRatherThanExplaining) {
  // Below the observation threshold nothing is said either way. The counts are in
  // front of the reader -- a run of four hits renders all four values -- so a
  // sentence declining to call one of them rare costs more than it says.
  Aggregator Aggregate;
  for (uint64_t Seq = 0; Seq < Aggregator::MinObservationsForOutliers - 1; ++Seq)
    Aggregate.Record("loop", "id", ValueName(Seq), /*IsDocument=*/false, Seq,
                     Seq);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "id");
  ASSERT_NE(Fields, nullptr);
  EXPECT_EQ(Fields->get("outliers"), nullptr) << ToString(*Fields);
  EXPECT_EQ(Fields->get("outliers_of"), nullptr) << ToString(*Fields);
}

TEST(AggregateTest, ARareValueSurvivesALongTailOfLessRareOnes) {
  // The gate is on repetition over the whole run rather than on any one value,
  // so a capture that mostly repeats still reports the odd one out even with
  // enough varied values beside it to pull the average down. Rarity decides the
  // order, so the value seen once outranks a tail seen twice however late it
  // arrives -- which is what keeps a bound from dropping it.
  Aggregator Aggregate;
  uint64_t Seq = 0;
  for (; Seq < 60; ++Seq)
    Aggregate.Record("loop", "kind", "common", /*IsDocument=*/false, Seq, Seq);
  for (unsigned I = 0; I < 20; ++I)
    for (unsigned Twice = 0; Twice < 2; ++Twice, ++Seq)
      Aggregate.Record("loop", "kind", ValueName(I), /*IsDocument=*/false, Seq, Seq);
  Aggregate.Record("loop", "kind", "wedged", /*IsDocument=*/false, Seq, Seq);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "kind");
  ASSERT_NE(Fields, nullptr);
  const llvm::json::Array *Outliers = Fields->getArray("outliers");
  ASSERT_NE(Outliers, nullptr);
  ASSERT_FALSE(Outliers->empty());
  EXPECT_EQ((*Outliers)[0].getAsObject()->getString("value"),
            std::optional<llvm::StringRef>("wedged"));
}

TEST(AggregateTest, OutliersUpToTheBoundSayNothingWasDropped) {
  Aggregator Aggregate;
  size_t Seq = 0;
  for (; Seq < Aggregator::MaxOutliers; ++Seq)
    Aggregate.Record("loop", "p", ValueName(Seq), /*IsDocument=*/false, Seq, Seq);
  // Enough common hits to put the run over the threshold at which a rare value
  // is worth naming at all.
  for (size_t I = 0; I < Aggregator::MinObservationsForOutliers; ++I, ++Seq)
    Aggregate.Record("loop", "p", "common", /*IsDocument=*/false, Seq, Seq);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "p");
  ASSERT_NE(Fields, nullptr);
  const llvm::json::Array *Outliers = Fields->getArray("outliers");
  ASSERT_NE(Outliers, nullptr);
  EXPECT_EQ(Outliers->size(), Aggregator::MaxOutliers);
  EXPECT_EQ(Fields->get("outliers_elided"), nullptr);
}

TEST(AggregateTest, ADocumentValuedCaptureKeepsItsStructure) {
  // The shape a composite capture arrives in. Keyed as text, because that is what
  // lets an emission mode and a comparison decide "the same value" by comparing
  // strings, but rendered as the document it came from: as a key it would be
  // escaped by the serialization that writes the response, and a caller would
  // have to undo the escaping by hand to read a captured name.
  Aggregator Aggregate;
  const llvm::StringRef Exit = R"({"Data":{"summary":"\"exit\""}})";
  for (uint64_t Hit = 0; Hit < 3; ++Hit)
    Aggregate.Record("bb", "BB->getName()", Exit, /*IsDocument=*/true, Hit, Hit);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Value *Entry = FindCapture(Summary, "bb", "BB->getName()");
  ASSERT_NE(Entry, nullptr);
  // Not a string: a single-valued capture usually collapses to "value xN", and
  // a document has nowhere to put the count and stay readable as one.
  EXPECT_FALSE(Entry->getAsString().has_value()) << ToString(*Entry);
  const llvm::json::Object *Fields = Entry->getAsObject();
  ASSERT_NE(Fields, nullptr);
  EXPECT_EQ(Fields->getInteger("count"), 3);
  const llvm::json::Value *Value = Fields->get("value");
  ASSERT_NE(Value, nullptr);
  EXPECT_EQ(ToString(*Value), R"({"Data":{"summary":"\"exit\""}})");
}

TEST(AggregateTest, ADocumentValuedHistogramIsAListRatherThanKeys) {
  Aggregator Aggregate;
  Aggregate.Record("bb", "name", R"({"value":"a"})", /*IsDocument=*/true, 1, 1);
  Aggregate.Record("bb", "name", R"({"value":"a"})", /*IsDocument=*/true, 2, 2);
  Aggregate.Record("bb", "name", R"({"value":"b"})", /*IsDocument=*/true, 3, 3);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "bb", "name");
  ASSERT_NE(Fields, nullptr);
  EXPECT_EQ(Fields->getInteger("distinct"), 2);

  // A list, because a document cannot be a key without being escaped into one.
  const llvm::json::Array *Values = Fields->getArray("values");
  ASSERT_NE(Values, nullptr) << ToString(*Fields);
  EXPECT_EQ(ToString(*Values),
            R"([{"count":2,"value":{"value":"a"}},)"
            R"({"count":1,"value":{"value":"b"}}])");

  // Both sides of a transition are the document too, since a caller acting on
  // "this went from a to b" needs to read a and b.
  const llvm::json::Array *Transitions = Fields->getArray("transitions");
  ASSERT_NE(Transitions, nullptr);
  EXPECT_EQ(ToString(*Transitions),
            R"([{"count":1,"first_seq":3,"from":{"value":"a"},)"
            R"("to":{"value":"b"}}])");
}

TEST(AggregateTest, AScalarHistogramStaysKeyedByItsValue) {
  // The path nearly every capture takes, asserted beside the document case so
  // that keeping the structure cannot quietly cost the scalar rendering its
  // density.
  Aggregator Aggregate;
  Aggregate.Record("loop", "n", "7", /*IsDocument=*/false, 1, 1);
  Aggregate.Record("loop", "n", "8", /*IsDocument=*/false, 2, 2);

  const llvm::json::Value Summary = Aggregate.Render();
  const llvm::json::Object *Fields = FindFields(Summary, "loop", "n");
  ASSERT_NE(Fields, nullptr);
  const llvm::json::Object *Values = Fields->getObject("values");
  ASSERT_NE(Values, nullptr) << ToString(*Fields);
  EXPECT_EQ(ToString(*Values), R"({"7":1,"8":1})");
}
