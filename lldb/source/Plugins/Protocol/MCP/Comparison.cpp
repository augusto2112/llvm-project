//===- Comparison.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Comparison.h"
#include "ObservationEngine.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace lldb_private::mcp;
using namespace llvm;

namespace {

std::string Render(const json::Value &V) {
  std::string S;
  raw_string_ostream OS(S);
  OS << V;
  return S;
}

std::string Truncate(std::string S) {
  if (S.size() > MaxComparedSummaryChars) {
    S.resize(MaxComparedSummaryChars);
    S += "...";
  }
  return S;
}

/// One thing compared across runs: what each run said about it, keyed by label.
///
/// Held as text because that is what makes "the same" decidable for values of
/// every shape the runs can produce -- a number, a string, a summary object --
/// without a comparison per shape.
struct Compared {
  std::map<std::string, std::string> ByLabel;

  /// Whether every run that has a value for this said the same thing. A run that
  /// is missing it disagrees: an observation that resolved in one run and not in
  /// another is a difference, and the commonest one there is.
  bool Agrees(size_t Runs) const {
    if (ByLabel.size() != Runs)
      return false;
    const std::string &First = ByLabel.begin()->second;
    return all_of(ByLabel, [&](const auto &Entry) {
      return Entry.second == First;
    });
  }
};

/// Everything compared, in the order it is reported.
class Table {
public:
  void Add(StringRef Name, StringRef Label, std::string Value) {
    if (!m_index.count(Name.str())) {
      m_index[Name.str()] = m_order.size();
      m_order.push_back(Name.str());
    }
    m_rows[Name.str()].ByLabel[Label.str()] = std::move(Value);
  }

  /// Names in the order they were added, so that a report follows the plan rather
  /// than the alphabet.
  ArrayRef<std::string> Names() const { return m_order; }
  const Compared &Row(StringRef Name) const { return m_rows.at(Name.str()); }

private:
  std::vector<std::string> m_order;
  std::map<std::string, size_t> m_index;
  std::map<std::string, Compared> m_rows;
};

/// A hit's capture tuple as the values it holds, keyed by the expression each came
/// from.
///
/// The tuple is stored as one string joined by a control character, which is what
/// makes two tuples comparable as text. That is the wrong thing to hand back: a
/// reader wants to know which capture differed, and the separator has no business
/// in a document at all.
json::Value DescribeTuple(StringRef Tuple, ArrayRef<CaptureReport> Captures) {
  SmallVector<StringRef, 8> Values;
  Tuple.split(Values, CaptureTupleSeparator);
  // The join leaves a trailing empty field, and a tuple of no captures is one
  // empty field.
  while (!Values.empty() && Values.back().empty())
    Values.pop_back();

  if (Values.size() != Captures.size()) {
    // Rather than key values on the wrong names. The counts differ when a capture
    // was turned off partway through the run, so the tuple of an early hit holds
    // more fields than the report has live captures.
    json::Array Out;
    for (StringRef Value : Values)
      Out.push_back(Truncate(Value.str()));
    return Out;
  }

  json::Object Out;
  for (const auto &[Capture, Value] : zip_equal(Captures, Values))
    Out[Capture.Expr] = Truncate(Value.str());
  return Out;
}

/// The first index at which the tuples of two runs differ, or none when one is a
/// prefix of the other.
std::optional<size_t> FirstDifference(ArrayRef<std::string> A,
                                      ArrayRef<std::string> B) {
  const size_t Shared = std::min(A.size(), B.size());
  for (size_t I = 0; I < Shared; ++I)
    if (A[I] != B[I])
      return I;
  return std::nullopt;
}

const ObservationReport *FindObservation(const ObservationResult &Result,
                                         StringRef Label) {
  for (const ObservationReport &Report : Result.Observations)
    if (Report.Label == Label)
      return &Report;
  return nullptr;
}

} // namespace

json::Value lldb_private::mcp::CompareRuns(ArrayRef<ComparedRun> Runs) {
  json::Array Rows;
  Table Compare;

  // Every label observed by any run, in the order the first run that has them
  // reports them, so that a run which failed to resolve one does not renumber the
  // rest.
  std::vector<std::string> Labels;
  std::set<std::string> Seen;
  for (const ComparedRun &Run : Runs) {
    if (!Run.Result)
      continue;
    for (const ObservationReport &Report : Run.Result->Observations)
      if (Seen.insert(Report.Label).second)
        Labels.push_back(Report.Label);
  }

  for (const ComparedRun &Run : Runs) {
    json::Object Row{{"label", Run.Label}};
    if (!Run.Result) {
      Row["error"] = Run.Error;
      Rows.push_back(std::move(Row));
      continue;
    }
    const ObservationResult &R = *Run.Result;
    Row["outcome"] = ToString(R.Result);
    Row["elapsed_ms"] = R.ElapsedMs;
    if (!R.Terminal.Description.empty())
      Row["ended"] = R.Terminal.Description;

    json::Object Hits;
    for (const ObservationReport &Report : R.Observations)
      Hits[Report.Label] = static_cast<int64_t>(Report.Hits);
    if (!Hits.empty())
      Row["hits"] = std::move(Hits);
    Rows.push_back(std::move(Row));

    // How it ended, and where. A fix that turns a hang into an exit is the whole
    // answer and belongs at the top of the comparison.
    Compare.Add("outcome", Run.Label, ToString(R.Result).str());
    Compare.Add("ended", Run.Label, R.Terminal.Description);
    if (!R.Terminal.Function.empty())
      Compare.Add("ended_in", Run.Label, R.Terminal.Function);

    for (const std::string &Label : Labels) {
      const ObservationReport *Report = FindObservation(R, Label);
      if (!Report)
        continue;
      Compare.Add(Label + ".resolved_locations", Run.Label,
                  std::to_string(Report->ResolvedLocations));
      Compare.Add(Label + ".hits", Run.Label, std::to_string(Report->Hits));
      Compare.Add(Label + ".emitted", Run.Label,
                  std::to_string(Report->Emitted));
      if (Report->ResolutionError)
        Compare.Add(Label + ".error", Run.Label, *Report->ResolutionError);
      for (const CaptureReport &Capture : Report->Captures)
        Compare.Add(Label + "." + Capture.Expr, Run.Label,
                    Truncate(Render(Capture.Render())));
    }

    // Rendered per run rather than compared, since two runs' aggregates of one
    // capture differ in shape as often as in content and the summary is what a
    // reader wants either way.
    if (const json::Object *Agg = R.Aggregate.getAsObject())
      for (const auto &[Label, Captures] : *Agg)
        if (const json::Object *ByCapture = Captures.getAsObject())
          for (const auto &[Capture, Summary] : *ByCapture)
            Compare.Add(Label.str() + " summary of " + Capture.str(), Run.Label,
                        Truncate(Render(Summary)));
  }

  json::Object Diverged;
  json::Array Agreed;
  for (const std::string &Name : Compare.Names()) {
    const Compared &Row = Compare.Row(Name);
    if (Row.Agrees(Runs.size())) {
      Agreed.push_back(Name);
      continue;
    }
    json::Object Sides;
    for (const auto &[Label, Value] : Row.ByLabel)
      Sides[Label] = Value;
    Diverged[Name] = std::move(Sides);
  }

  // The hit at which two runs stopped agreeing, which for a miscompile or a hang
  // is the answer: everything before it is the same computation and everything
  // after is a consequence.
  json::Object FirstDiverged;
  if (Runs.size() >= 2 && Runs.front().Result && Runs.back().Result) {
    const ComparedRun &A = Runs.front();
    const ComparedRun &B = Runs.back();
    for (const std::string &Label : Labels) {
      const ObservationReport *LHS = FindObservation(*A.Result, Label);
      const ObservationReport *RHS = FindObservation(*B.Result, Label);
      if (!LHS || !RHS || LHS->HitTuples.empty() || RHS->HitTuples.empty())
        continue;

      json::Object Entry;
      std::optional<size_t> At =
          FirstDifference(LHS->HitTuples, RHS->HitTuples);
      if (!At) {
        // A prefix of the other is not a disagreement about any hit: one run just
        // kept going. Reported as the count, because that is the difference.
        if (LHS->HitTuples.size() == RHS->HitTuples.size() &&
            LHS->HitTuplesDropped == 0 && RHS->HitTuplesDropped == 0)
          continue;
        Entry["identical_through"] =
            static_cast<int64_t>(std::min(LHS->HitTuples.size(),
                                          RHS->HitTuples.size()));
        if (LHS->HitTuplesDropped != 0 || RHS->HitTuplesDropped != 0)
          Entry["beyond_that"] =
              "not compared; more hits were taken than are kept for comparison";
      } else {
        Entry["hit"] = static_cast<int64_t>(*At + 1);
        json::Object Saw;
        Saw[A.Label] = DescribeTuple(LHS->HitTuples[*At], LHS->Captures);
        Saw[B.Label] = DescribeTuple(RHS->HitTuples[*At], RHS->Captures);
        Entry["saw"] = std::move(Saw);
      }
      FirstDiverged[Label] = std::move(Entry);
    }
  }

  json::Object Out{{"runs", std::move(Rows)}};
  if (!Diverged.empty())
    Out["diverged"] = std::move(Diverged);
  if (!FirstDiverged.empty())
    Out["first_divergent_hit"] = std::move(FirstDiverged);
  // Names alone. What a caller asks a comparison is whether anything else moved,
  // and the answer is a list of what did not rather than the values that did not.
  if (!Agreed.empty())
    Out["agreed"] = std::move(Agreed);
  return Out;
}
