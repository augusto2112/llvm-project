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
  /// on it lands on the hit it named.
  void Record(llvm::StringRef Label, llvm::StringRef Capture,
              llvm::StringRef RenderedValue, uint64_t Seq, uint64_t Hit);

  /// Renders an object of labels, each an object of captures.
  ///
  /// A capture whose value never varied collapses to a string, `"false x4012"`.
  /// Most captures are constant over a run, so that collapse is where most of
  /// the saving is. One that varied gets an object carrying `distinct`,
  /// `values`, `transitions`, and `outliers` where a value is rare enough among
  /// enough hits to be worth naming.
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

  /// Histogram keys kept in `values`. A capture that renders a distinct string
  /// on every hit would otherwise put one key per hit into the response, so the
  /// most frequent are kept and the remainder is reported as a count. Outliers
  /// are unaffected: they are selected before this bound applies, which is what
  /// keeps the rare value that is usually the answer from being the one
  /// dropped.
  static constexpr size_t MaxHistogramValues = 32;

private:
  struct ValueStats {
    uint64_t Count = 0;

    /// The sequence number of the hit that first showed this value, which is
    /// where a reader goes to see a rare one in context.
    uint64_t FirstSeq = 0;
    uint64_t FirstHit = 0;
  };

  /// A change of value, attributed to the hit that first showed the new one.
  struct Transition {
    uint64_t Seq = 0;
    std::string From;
    std::string To;
  };

  struct CaptureSummary {
    /// Hits recorded, which is the sum of the value counts.
    uint64_t Total = 0;

    /// Counts keyed by the rendered value.
    std::map<std::string, ValueStats> Values;

    std::vector<Transition> Transitions;

    /// The value at the most recent hit, against which the next one is
    /// compared.
    std::string Last;
  };

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
