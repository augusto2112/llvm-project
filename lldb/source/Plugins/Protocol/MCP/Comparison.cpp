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
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <cmath>
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

  /// Whether every run that has a value for this said the same thing, against
  /// \p Runs runs that could say anything at all. A run that is missing it
  /// disagrees: an observation that resolved in one run and not in another is a
  /// difference, and the commonest one there is.
  ///
  /// \p Runs counts the runs that produced a result rather than the runs asked
  /// for. A run that could not be launched contributes to no row, so counting it
  /// here would put every row one short of the total and report the whole of the
  /// surviving run's report as a one-sided divergence -- which is the opposite of
  /// the promise that the other runs still answer.
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

/// How a capture resolved, or nothing when it resolved the way nearly every
/// capture does and has nothing to say.
///
/// What a capture reports about itself in its own run is a cost account --
/// `{"evaluations":1,"tier":"expression","total_ms":599.88}` -- and a timing in a
/// comparison makes every expression-tier capture diverge on the clock alone.
/// Measured at 165 of the 803 chars of one comparison, spent on two slightly
/// different timings, under a name a reader takes for the value of the
/// expression. A capture that failed identically in both runs was reported as a
/// divergence for the same reason.
///
/// Two runs of one plan resolve their captures the same way or they do not, and
/// that is the comparable fact: which mechanism read the value, whether anything
/// failed, whether a fixit rewrote the expression, whether cost control gave up
/// on it. How long it took is a property of the machine.
std::optional<std::string> DescribeResolution(const CaptureReport &Capture) {
  // `$return` is read out of the ABI's result location rather than resolved from
  // a name, so it has no tier and nothing that can fail.
  if (Capture.FromABI)
    return std::nullopt;

  const bool Clean =
      Capture.Errors == 0 && Capture.FixedExpr.empty() && !Capture.Disabled;

  // A capture that resolved as a path and never failed is most captures, and a
  // row saying so in both runs is a name in `agreed` earning nothing.
  if (Clean && Capture.Tier == lldb_private::ValueResolutionTier::VariablePath)
    return std::nullopt;

  // Never evaluated is not the same as could not be read: the observation may
  // never have been hit. The default tier spells itself "unavailable", which is
  // the word a capture that genuinely failed gets, so naming the tier here would
  // collapse the one distinction the rest of the report keeps.
  if (Clean && Capture.Evaluations == 0)
    return "not_evaluated";

  if (Clean)
    return ToString(Capture.Tier).str();

  json::Object O{{"tier", ToString(Capture.Tier)}};
  // The run is not evaluating what the caller wrote, and beside the key -- which
  // is the caller's own spelling -- this is what connects the two.
  if (!Capture.FixedExpr.empty())
    O["fixed_as"] = Capture.FixedExpr;
  // Whether, not how many: the count is a number of hits, and two runs that both
  // failed to read a name differ in it whenever they differ in how far they got.
  if (Capture.Errors != 0)
    O["errors"] = true;
  if (Capture.Disabled)
    O["disabled"] = true;
  return Render(std::move(O));
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
    // Whole milliseconds. `llvm::json` prints a double at max_digits10, so an
    // elapsed 358.432 goes into the document as 358.43200000000002 and no amount
    // of rounding beforehand changes that -- the only shape that prints short is
    // an integer. Nothing is lost: the same binary under the same plan measured
    // 182 ms and 558 ms in one session, so a fraction of a millisecond is a
    // digit of noise reported to the width of a measurement.
    Row["elapsed_ms"] = static_cast<int64_t>(std::llround(R.ElapsedMs));
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
    // The status is the difference in the commonest before-and-after there is: a
    // compiler that starts exiting non-zero has the same outcome, the same
    // description and the same everything else, so leaving this out reported a
    // pass/fail pair as no difference at all.
    if (R.Terminal.ExitStatus)
      Compare.Add("exit_status", Run.Label,
                  std::to_string(*R.Terminal.ExitStatus));
    Compare.Add("ended", Run.Label, R.Terminal.Description);
    if (!R.Terminal.Function.empty())
      Compare.Add("ended_in", Run.Label, R.Terminal.Function);
    // The line, without which a crashed run gives a signal and a function name
    // and leaves the caller to spend a second call finding out where. The path is
    // reduced to its filename because two runs of the same source built in
    // different directories differ in every character of the prefix and in
    // nothing that matters.
    if (R.Terminal.Line != 0)
      Compare.Add("ended_at", Run.Label,
                  formatv("{0}:{1}", sys::path::filename(R.Terminal.File),
                          R.Terminal.Line)
                      .str());

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
        if (std::optional<std::string> How = DescribeResolution(Capture))
          Compare.Add(Label + "." + Capture.Expr + ".capture", Run.Label,
                      std::move(*How));
    }

    // What the capture was observed to hold, under the dotted name a reader would
    // guess for it. This is the row that carries the answer -- `320 x1` against
    // `282 x1` -- and it was spelled `<label> summary of <expr>`: a phrase that
    // cannot be named as a path, sorts away from the siblings it belongs beside,
    // and costs eleven characters per capture to say what the position in the
    // object already says. The metadata had the dotted name; they are the other
    // way round now.
    if (const json::Object *Agg = R.Aggregate.getAsObject())
      for (const auto &[Label, Captures] : *Agg)
        if (const json::Object *ByCapture = Captures.getAsObject())
          for (const auto &[Capture, Summary] : *ByCapture)
            Compare.Add(Label.str() + "." + Capture.str(), Run.Label,
                        Truncate(Render(Summary)));
  }

  // Against the runs that answered, not the runs asked for.
  const size_t Answered = count_if(
      Runs, [](const ComparedRun &Run) { return Run.Result.has_value(); });

  json::Object Diverged;
  json::Array Agreed;
  for (const std::string &Name : Compare.Names()) {
    const Compared &Row = Compare.Row(Name);
    if (Row.Agrees(Answered)) {
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
