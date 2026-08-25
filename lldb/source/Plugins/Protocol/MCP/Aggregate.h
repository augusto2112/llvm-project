//===- Aggregate.h --------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_PROTOCOL_MCP_AGGREGATE_H
#define LLDB_SOURCE_PLUGINS_PROTOCOL_MCP_AGGREGATE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lldb_private::mcp {

/// What each capture was observed to hold, kept per label and capture
/// expression.
///
/// Record is called once per hit, including the hits whose events an emission
/// mode drops. Reducing the event stream is a saving rather than a loss only
/// because the aggregate still sees every hit, so this class holds no
/// filtering of its own and knows nothing about emission.
class Aggregator {
public:
  /// Accumulates one hit. Values are compared as opaque strings, so the
  /// rendering decides what counts as a change: one carrying an address or a
  /// timestamp makes every hit a transition. Hits are expected in run order,
  /// since transitions chain consecutive calls.
  /// \p Seq numbers the whole event stream, which is how a transition is
  /// matched to a line in the artifact. \p Hit numbers this observation's own
  /// hits, which is what only_hit takes: the two differ as soon as a plan holds
  /// more than one observation, and an outlier reports the hit so that acting
  /// on it lands on the hit it named. Whoever records has to number \p Hit the
  /// way only_hit reads it, skipped hits included, or that is exactly what it
  /// will not do.
  /// \p IsDocument says that \p RenderedValue is a serialized JSON document
  /// rather than a scalar, which is what \ref Render needs in order to put the
  /// document back into the response instead of leaving it as an escaped string.
  /// Recorded rather than guessed from the text, because a program's own string
  /// value may be spelled like a document and a number is spelled like one every
  /// time.
  void Record(llvm::StringRef Label, llvm::StringRef Capture,
              llvm::StringRef RenderedValue, bool IsDocument, uint64_t Seq,
              uint64_t Hit);

  /// Renders an object of labels, each an object of captures.
  ///
  /// A capture whose value never varied collapses to a string, `"false x4012"`.
  /// Most captures are constant over a run, so that collapse is where most of
  /// the saving is. One that varied gets an object carrying `distinct`,
  /// `values`, `transitions`, and `outliers` where a value is rare enough among
  /// enough hits to be worth naming.
  ///
  /// Every field is a summary of all the hits rather than a prefix of them.
  /// A prefix is the wrong shape for what a caller asks a summary: the first
  /// thirty-two changes of a value that changes constantly describe the start
  /// of the run, and the question is almost always about the whole of it.
  ///
  /// The same recorded hits always render the same text: json::Object sorts its
  /// keys when printed, and every array is ordered by an explicit key.
  llvm::json::Value Render() const;

  /// A value seen once or twice is only remarkable among many, and below this
  /// many observations there is no "many": a run holding two values reports
  /// both of them as outliers, which adds nothing to `values`.
  static constexpr uint64_t MinObservationsForOutliers = 20;

  /// The most times a value can be seen and still be rare.
  static constexpr uint64_t MaxOutlierCount = 2;

  /// Whether a value seen once or twice is remarkable, decided against the run it
  /// sits in: what a hit picked at random out of this capture holds. If that value
  /// was itself seen once or twice then being seen once or twice is the norm here
  /// and nothing about it is worth naming.
  ///
  /// Two measurements shaped this. A capture of a compiler's node ids over 1808
  /// hits, all distinct: every value was rare, and the list rendered 32 of them
  /// plus a count of 1776 more -- three kilobytes saying only that ids differ,
  /// which `distinct` already said. Then a capture of an instruction pointer over
  /// 1200 hits: three addresses held 135, 133 and 133 of them and four hundred
  /// held one or two, so a gate asking whether anything was common passed, and the
  /// answer was still eight arbitrary addresses with 392 elided. Two thirds of
  /// those hits were in the tail, which is what makes the tail ordinary. The
  /// counterexample the rule has to keep is one vector type among four thousand
  /// integers, where the typical hit holds the common value and the rare one is the
  /// answer.
  static bool RarityIsMeaningful(
      llvm::ArrayRef<std::pair<std::string, uint64_t>> ByCountDescending,
      uint64_t Total);

  /// Histogram keys kept in `values`. A capture that renders a distinct string
  /// on every hit would otherwise put one key per hit into the response, so the
  /// most frequent are kept and the remainder is reported as a count. Outliers
  /// are unaffected: they are selected before this bound applies, which is what
  /// keeps the rare value that is usually the answer from being the one
  /// dropped.
  ///
  /// Small, because the values worth naming individually are the frequent few
  /// and the rare few, and `outliers` covers the second group. Past the first
  /// handful a histogram of a high-cardinality capture is a list of equally
  /// common values whose only content is their number.
  static constexpr size_t MaxHistogramValues = 8;

  /// Distinct transitions kept, ranked by how often each occurred. Recording
  /// every change in order and keeping the first N of them summarised an
  /// oscillation as N copies of the same two edges: measured on a capture that
  /// cycled through four values, 32 entries reading 3->2, 2->1, 1->0, 0->3 over
  /// and over, with 7959 more elided. The distinct edges with their counts say
  /// the same thing in four.
  static constexpr size_t MaxTransitions = 8;

  /// Outliers kept. A capture that renders a distinct value at every hit -- an
  /// address, a pointer -- makes every one of those values rare, so an
  /// unbounded outlier list is the same unboundedness the histogram cap exists
  /// to prevent, reached by the other door. The rarest are kept, and among
  /// equally rare values the earliest, which is the order they are already
  /// ranked in: a bound that dropped the first sighting would defeat the point
  /// of reporting one.
  static constexpr size_t MaxOutliers = 8;

private:
  struct ValueStats {
    uint64_t Count = 0;

    /// Whether the key this is filed under is a serialized document. See
    /// \ref Record.
    bool Document = false;

    /// The sequence number of the hit that first showed this value, which is
    /// where a reader goes to see a rare one in context.
    uint64_t FirstSeq = 0;
    uint64_t FirstHit = 0;
  };

  /// A change of value, counted over the run. The pair is what the reader acts
  /// on -- "this went from legal to custom, 900 times" -- and counting the
  /// distinct pairs rather than listing every change also bounds what a run of
  /// millions of hits holds in memory.
  struct EdgeStats {
    uint64_t Count = 0;

    /// The sequence number of the first hit that made this change, which is the
    /// line of the artifact a reader goes to for it.
    uint64_t FirstSeq = 0;
  };

  struct CaptureSummary {
    /// Hits recorded, which is the sum of the value counts.
    uint64_t Total = 0;

    /// Counts keyed by the rendered value.
    std::map<std::string, ValueStats> Values;

    /// Counts keyed by the pair moved between.
    std::map<std::pair<std::string, std::string>, EdgeStats> Transitions;

    /// The value at the most recent hit, against which the next one is
    /// compared.
    std::string Last;
  };

  /// One recorded value as the response should show it: the document it came
  /// from where the key is one, and the key itself otherwise.
  ///
  /// Recovered by parsing rather than by having been kept, so that a capture
  /// rendering something distinct at every hit does not hold two copies of every
  /// one of them. Parsing cannot fail on a key this class was given; a key that
  /// somehow does not parse falls back to the text, which is the previous
  /// behaviour rather than a lost value.
  static llvm::json::Value Displayed(llvm::StringRef Key, bool Document);

  static llvm::json::Value RenderCapture(const CaptureSummary &Summary);

  std::map<std::string, std::map<std::string, CaptureSummary>> m_labels;
};

/// A block of locations that the end of an event stream repeats.
struct CycleReport {
  unsigned Period;
  unsigned Repeats;
  std::vector<std::string> Sequence;
};

/// The longest repeating block DetectCycle looks for. A block longer than this
/// is not a loop shape a caller can act on, and the search cost grows with the
/// bound.
constexpr unsigned MaxCyclePeriod = 32;

/// Finds the shortest block that the end of \p TailLabels repeats, where each
/// entry names the location an event came from.
///
/// The block has to reach the final entry, so that what is reported is where
/// the program is now rather than somewhere it has already left, and it has to
/// repeat at least three times, since two repetitions of a block are as much a
/// coincidence as a loop. For a run that never terminates this is the answer
/// the caller reports, which makes a cycle claimed in error worse than one
/// missed.
std::optional<CycleReport> DetectCycle(llvm::ArrayRef<std::string> TailLabels);

} // namespace lldb_private::mcp

#endif
