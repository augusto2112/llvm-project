//===- Aggregate.cpp ------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Aggregate.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace lldb_private::mcp;
using namespace llvm;

void Aggregator::Record(StringRef Label, StringRef Capture,
                        StringRef RenderedValue, bool IsDocument, uint64_t Seq,
                        uint64_t Hit) {
  CaptureSummary &Summary = m_labels[Label.str()][Capture.str()];
  std::string Rendered = RenderedValue.str();

  if (Summary.Total != 0 && Summary.Last != Rendered) {
    EdgeStats &Edge = Summary.Transitions[{Summary.Last, Rendered}];
    if (Edge.Count == 0)
      Edge.FirstSeq = Seq;
    ++Edge.Count;
  }

  ++Summary.Total;
  ValueStats &Stats = Summary.Values[Rendered];
  if (Stats.Count == 0) {
    Stats.FirstSeq = Seq;
    Stats.FirstHit = Hit;
    Stats.Document = IsDocument;
  }
  ++Stats.Count;
  Summary.Last = std::move(Rendered);
}

json::Value Aggregator::Displayed(StringRef Key, bool Document) {
  if (!Document)
    return Key.str();
  if (Expected<json::Value> Parsed = json::parse(Key))
    return std::move(*Parsed);
  else
    consumeError(Parsed.takeError());
  return Key.str();
}

namespace {

/// How many of \p Ranked, already ordered by descending count, are worth showing
/// when \p Total of them exist.
///
/// A bounded list of counted things answers "which of these dominate". Where
/// nothing dominates -- every entry kept carries the same count -- the list has
/// no answer to give, and each entry past the first repeats what the first said.
/// That only justifies shortening it further when most of the population is
/// being dropped anyway: a histogram of nine equally common opcodes is worth
/// enumerating, and one of two thousand equally common node ids is a sample
/// whose only content is the count beside it.
template <typename T, typename CountOf>
size_t WorthShowing(llvm::ArrayRef<T> Ranked, size_t Total, CountOf Count) {
  if (Ranked.size() < 2 || Total - Ranked.size() < Ranked.size())
    return Ranked.size();
  const uint64_t First = Count(Ranked.front());
  for (const T &Entry : Ranked.drop_front())
    if (Count(Entry) != First)
      return Ranked.size();
  return 1;
}

} // namespace

bool Aggregator::RarityIsMeaningful(
    ArrayRef<std::pair<std::string, uint64_t>> ByCountDescending,
    uint64_t Total) {
  // The count of the value a hit picked at random out of the run would hold.
  // Walking the values from the most common down, the one that crosses half the
  // hits is that value, which is the hit-weighted median: the unweighted median
  // would be a fact about the length of the tail rather than about the run, and
  // would call one vector type among four thousand integers unremarkable because
  // half the *values* are the rare one.
  uint64_t Seen = 0;
  for (const auto &[Value, Count] : ByCountDescending) {
    Seen += Count;
    if (Seen * 2 >= Total)
      return Count > MaxOutlierCount;
  }
  return false;
}

json::Value Aggregator::RenderCapture(const CaptureSummary &Summary) {
  // An object describing a single value is mostly punctuation, and a capture
  // that never varied is the common case.
  if (Summary.Values.size() == 1) {
    const std::pair<const std::string, ValueStats> &Only =
        *Summary.Values.begin();
    if (!Only.second.Document)
      return formatv("{0} x{1}", Only.first, Only.second.Count).str();
    // A document cannot go in the string: it is what "{0}" would escape. The
    // count moves out beside it, since there is nowhere in a document to put an
    // ` x3` and keep it readable as one.
    return json::Object{{"value", Displayed(Only.first, /*Document=*/true)},
                        {"count", Only.second.Count}};
  }

  // A capture rendering something unique per hit -- an address, a pointer --
  // yields one histogram key per hit, which alone can exceed the response
  // budget. Keep the most frequent values and say how many were dropped, since
  // a silently shortened histogram reads as a complete one.
  std::vector<std::pair<std::string, uint64_t>> ByCount;
  ByCount.reserve(Summary.Values.size());
  for (const auto &[Rendered, Stats] : Summary.Values)
    ByCount.emplace_back(Rendered, Stats.Count);
  llvm::stable_sort(ByCount, [](const auto &LHS, const auto &RHS) {
    return std::tie(RHS.second, LHS.first) < std::tie(LHS.second, RHS.first);
  });

  size_t Kept = std::min<size_t>(ByCount.size(), MaxHistogramValues);
  Kept = WorthShowing(ArrayRef(ByCount).take_front(Kept), ByCount.size(),
                      [](const auto &Entry) { return Entry.second; });

  auto DocumentOf = [&Summary](StringRef Key) {
    auto It = Summary.Values.find(Key.str());
    return It != Summary.Values.end() && It->second.Document;
  };

  // An object keyed by the value, unless any value is a document. A document
  // used as a key is escaped by the serialization that writes it, which is what
  // turned a captured `StringRef` into a line of backslashes the caller had to
  // undo by hand; so where one is present the histogram becomes a list of
  // value-and-count pairs and each value keeps its shape. The object form is
  // kept for the scalar case because that is nearly every capture and it is half
  // the size.
  const bool AnyDocument =
      any_of(ArrayRef(ByCount).take_front(Kept),
             [&](const auto &Entry) { return DocumentOf(Entry.first); });

  json::Value Histogram = nullptr;
  if (AnyDocument) {
    json::Array Listed;
    for (size_t I = 0; I < Kept; ++I)
      Listed.push_back(
          json::Object{{"value", Displayed(ByCount[I].first,
                                           DocumentOf(ByCount[I].first))},
                       {"count", ByCount[I].second}});
    Histogram = std::move(Listed);
  } else {
    json::Object Keyed;
    for (size_t I = 0; I < Kept; ++I)
      Keyed[ByCount[I].first] = ByCount[I].second;
    Histogram = std::move(Keyed);
  }

  // Ranked by how often each change happened, so a bound drops the edges the
  // run took least often. A cycle is the shape this exists for: two values
  // alternating render as two edges carrying half the hits each, whatever the
  // length of the run.
  struct Edge {
    std::string From, To;
    uint64_t Count, FirstSeq;
  };
  std::vector<Edge> Edges;
  Edges.reserve(Summary.Transitions.size());
  for (const auto &[Pair, Stats] : Summary.Transitions)
    Edges.push_back({Pair.first, Pair.second, Stats.Count, Stats.FirstSeq});
  llvm::stable_sort(Edges, [](const Edge &LHS, const Edge &RHS) {
    return std::tie(RHS.Count, LHS.FirstSeq) < std::tie(LHS.Count, RHS.FirstSeq);
  });

  size_t Dropped = 0;
  const size_t EdgesTotal = Edges.size();
  size_t KeptEdges = std::min<size_t>(EdgesTotal, MaxTransitions);
  KeptEdges = WorthShowing(ArrayRef(Edges).take_front(KeptEdges), EdgesTotal,
                           [](const Edge &E) { return E.Count; });
  if (KeptEdges < EdgesTotal) {
    Dropped = EdgesTotal - KeptEdges;
    Edges.resize(KeptEdges);
  }

  // Selected by frequency and rendered in run order, because the order changes
  // happened in is part of what they say: "legal, then custom, then legal" is a
  // different story from the set of pairs it is built from.
  llvm::stable_sort(Edges, [](const Edge &LHS, const Edge &RHS) {
    return LHS.FirstSeq < RHS.FirstSeq;
  });

  json::Array Transitions;
  for (const Edge &Change : Edges)
    Transitions.push_back(
        json::Object{{"from", Displayed(Change.From, DocumentOf(Change.From))},
                     {"to", Displayed(Change.To, DocumentOf(Change.To))},
                     {"count", Change.Count},
                     {"first_seq", Change.FirstSeq}});

  json::Object Out{{"distinct", Summary.Values.size()},
                   {"values", std::move(Histogram)},
                   {"transitions", std::move(Transitions)}};

  // Reported beside the histogram rather than inside it. A note living among
  // the values shares their namespace, so a capture that rendered the note's
  // own key would have its count overwritten and lost with nothing to show
  // that it had been. A count is also more use to a reader than prose.
  if (Kept < ByCount.size())
    Out["values_elided"] = ByCount.size() - Kept;
  if (Dropped != 0)
    Out["transitions_elided"] = Dropped;

  // Two gates, because they exclude different runs. A run of a dozen hits has no
  // "many" for a value to be rare among, and a run whose typical hit holds a value
  // seen once or twice has nothing for a rare one to be rare against -- there,
  // every value is an outlier, which is another way of saying none is.
  if (Summary.Total >= MinObservationsForOutliers &&
      RarityIsMeaningful(ByCount, Summary.Total)) {
    struct Outlier {
      std::string Rendered;
      uint64_t Count;
      uint64_t FirstSeq;
      uint64_t FirstHit;
    };
    std::vector<Outlier> Rare;
    for (const auto &[Rendered, Stats] : Summary.Values)
      if (Stats.Count <= MaxOutlierCount)
        Rare.push_back({Rendered, Stats.Count, Stats.FirstSeq, Stats.FirstHit});

    // Rarest first, and ties broken by where the value was first seen, so the
    // array never depends on the order the values happen to be stored in.
    llvm::stable_sort(Rare, [](const Outlier &LHS, const Outlier &RHS) {
      return std::tie(LHS.Count, LHS.FirstSeq) <
             std::tie(RHS.Count, RHS.FirstSeq);
    });

    if (!Rare.empty()) {
      // Bounded like the histogram and for the same reason. The order above
      // puts the rarest first and breaks ties by first sighting, so the entries
      // a bound drops are the least rare and latest of them.
      const size_t KeptRare = std::min<size_t>(Rare.size(), MaxOutliers);
      json::Array Outliers;
      for (size_t I = 0; I < KeptRare; ++I)
        Outliers.push_back(json::Object{
            {"value", Displayed(Rare[I].Rendered, DocumentOf(Rare[I].Rendered))},
            {"count", Rare[I].Count},
            {"first_hit", Rare[I].FirstHit},
            {"first_seq", Rare[I].FirstSeq}});
      Out["outliers"] = std::move(Outliers);
      if (KeptRare < Rare.size())
        Out["outliers_elided"] = Rare.size() - KeptRare;
    }
  }

  return Out;
}

json::Value Aggregator::Render() const {
  json::Object Out;
  for (const auto &[Label, Captures] : m_labels) {
    json::Object Rendered;
    for (const auto &[Capture, Summary] : Captures)
      Rendered[Capture] = RenderCapture(Summary);
    Out[Label] = std::move(Rendered);
  }
  return Out;
}

std::optional<CycleReport>
lldb_private::mcp::DetectCycle(ArrayRef<std::string> TailEntries) {
  const size_t Count = TailEntries.size();

  // Ascending, because the shortest block that fits is the one describing the
  // loop: a longer block that also fits is that same loop counted in pairs.
  for (unsigned Period = MinCyclePeriod; Period <= MaxCyclePeriod; ++Period) {
    const size_t Length = Period;

    // Three repetitions of a longer block need more entries than there are,
    // and that only becomes truer as the period grows.
    if (Count < 3 * Length)
      break;

    ArrayRef<std::string> Block = TailEntries.take_back(Length);

    // A block of one entry repeated is the period-one claim reached by counting
    // the repetitions in twos, and it says nothing a hit count has not: a loop
    // getting somewhere sits at one location as readily as a wedged one.
    if (all_of(Block.drop_front(),
               [&](const std::string &Entry) { return Entry == Block.front(); }))
      continue;

    size_t Repeats = 1;
    while ((Repeats + 1) * Length <= Count &&
           TailEntries.slice(Count - (Repeats + 1) * Length, Length) == Block)
      ++Repeats;

    if (Repeats >= 3) {
      // The values an entry carries decided the repetition above and are not
      // reported: what a caller acts on is the block of locations, and the values
      // there are already in the aggregate and in the artifact's tail.
      std::vector<std::string> Sequence;
      Sequence.reserve(Block.size());
      for (const std::string &Entry : Block)
        Sequence.push_back(
            StringRef(Entry).split(CycleEntryValueSeparator).first.str());
      return CycleReport{Period, static_cast<unsigned>(Repeats),
                         std::move(Sequence)};
    }
  }

  return std::nullopt;
}
