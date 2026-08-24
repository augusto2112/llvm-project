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
                        StringRef RenderedValue, uint64_t Seq, uint64_t Hit) {
  CaptureSummary &Summary = m_labels[Label.str()][Capture.str()];
  std::string Rendered = RenderedValue.str();

  if (Summary.Total != 0 && Summary.Last != Rendered)
    Summary.Transitions.push_back({Seq, Summary.Last, Rendered});

  ++Summary.Total;
  ValueStats &Stats = Summary.Values[Rendered];
  if (Stats.Count == 0) {
    Stats.FirstSeq = Seq;
    Stats.FirstHit = Hit;
  }
  ++Stats.Count;
  Summary.Last = std::move(Rendered);
}

json::Value Aggregator::RenderCapture(const CaptureSummary &Summary) {
  // An object describing a single value is mostly punctuation, and a capture
  // that never varied is the common case.
  if (Summary.Values.size() == 1) {
    const std::pair<const std::string, ValueStats> &Only =
        *Summary.Values.begin();
    return formatv("{0} x{1}", Only.first, Only.second.Count).str();
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

  json::Object Histogram;
  const size_t Kept = std::min<size_t>(ByCount.size(), MaxHistogramValues);
  for (size_t I = 0; I < Kept; ++I)
    Histogram[ByCount[I].first] = ByCount[I].second;

  // Transitions arrive in run order, and the order is part of what they say.
  std::vector<Transition> Ordered = Summary.Transitions;
  llvm::stable_sort(Ordered, [](const Transition &LHS, const Transition &RHS) {
    return LHS.Seq < RHS.Seq;
  });
  // A capture that changes on nearly every hit produces a transition per hit,
  // which would leave the summary as large as the stream it summarises -- the
  // same unboundedness the histogram cap exists to prevent. The earliest are
  // kept, since the first change is usually where the story starts.
  json::Array Transitions;
  size_t Dropped = 0;
  if (Ordered.size() > MaxTransitions) {
    Dropped = Ordered.size() - MaxTransitions;
    Ordered.resize(MaxTransitions);
  }
  for (const Transition &Change : Ordered)
    Transitions.push_back(json::Object{
        {"seq", Change.Seq}, {"from", Change.From}, {"to", Change.To}});

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

  if (Summary.Total >= MinObservationsForOutliers) {
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
      json::Array Outliers;
      for (const Outlier &Value : Rare)
        Outliers.push_back(json::Object{{"value", Value.Rendered},
                                        {"count", Value.Count},
                                        {"first_hit", Value.FirstHit},
                                        {"first_seq", Value.FirstSeq}});
      Out["outliers"] = std::move(Outliers);
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
lldb_private::mcp::DetectCycle(ArrayRef<std::string> TailLabels) {
  const size_t Count = TailLabels.size();

  // Ascending, because the shortest block that fits is the one describing the
  // loop: a longer block that also fits is that same loop counted in pairs.
  for (unsigned Period = 1; Period <= MaxCyclePeriod; ++Period) {
    const size_t Length = Period;

    // Three repetitions of a longer block need more entries than there are,
    // and that only becomes truer as the period grows.
    if (Count < 3 * Length)
      break;

    ArrayRef<std::string> Block = TailLabels.take_back(Length);
    size_t Repeats = 1;
    while ((Repeats + 1) * Length <= Count &&
           TailLabels.slice(Count - (Repeats + 1) * Length, Length) == Block)
      ++Repeats;

    if (Repeats >= 3)
      return CycleReport{Period, static_cast<unsigned>(Repeats),
                         std::vector<std::string>(Block.begin(), Block.end())};
  }

  return std::nullopt;
}
