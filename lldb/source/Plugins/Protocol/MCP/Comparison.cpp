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

std::string Truncate(std::string S, unsigned Max = MaxComparedSummaryChars) {
  if (S.size() > Max) {
    S.resize(Max);
    S += "...";
  }
  return S;
}

/// \p Text with the carriage returns a terminal put in it removed.
///
/// A program observed here is launched on a pseudo-terminal, so every line it
/// wrote arrives ending `\r\n`. Left in, the return is a character of every line a
/// caller is shown, and it makes a stream captured on a pty differ from the same
/// text written down anywhere else.
std::string WithoutCarriageReturns(StringRef Text) {
  std::string Out;
  Out.reserve(Text.size());
  for (size_t I = 0; I < Text.size(); ++I)
    if (Text[I] != '\r' || (I + 1 != Text.size() && Text[I + 1] != '\n'))
      Out += Text[I];
  return Out;
}

/// \p Text as its lines, without the empty field a trailing newline leaves.
SmallVector<StringRef, 8> Lines(StringRef Text) {
  SmallVector<StringRef, 8> Out;
  Text.split(Out, '\n');
  if (!Out.empty() && Out.back().empty())
    Out.pop_back();
  return Out;
}

/// \p V with the text it holds bounded, rather than the document it serializes
/// to.
///
/// Cutting the serialization produced `{"short":"{\"distinct\":12,...,\"..."`: a
/// document sliced mid-token, which nothing can parse and which reads as though
/// the value itself ended there. A string is the only thing in a value that can be
/// shortened and leave a document behind, so it is the only thing shortened.
json::Value Bounded(const json::Value &V) {
  if (const json::Object *O = V.getAsObject()) {
    json::Object Out;
    for (const auto &[Key, Field] : *O)
      Out[Truncate(Key.str())] = Bounded(Field);
    return Out;
  }
  if (const json::Array *A = V.getAsArray()) {
    json::Array Out;
    for (const json::Value &Element : *A)
      Out.push_back(Bounded(Element));
    return Out;
  }
  if (std::optional<StringRef> S = V.getAsString())
    return Truncate(S->str());
  return V;
}

/// One run's answer for one compared thing.
struct Side {
  /// What "the same" is decided against. Text, because that makes agreement
  /// decidable for values of every shape a run can produce -- a number, a word, a
  /// summary object -- without a comparison per shape.
  std::string Key;

  /// What the caller is shown when the runs disagree, kept apart from the key
  /// because the key is the wrong thing to hand back. Serialized into the document
  /// as text, a scalar came back double-quoted (`"\"282 x1\""`) and a summary came
  /// back as a line of backslashes for the caller to undo by hand -- which is the
  /// complaint the capture aggregate exists to answer, reintroduced one level up.
  ///
  /// Null where what to show is derived from the key rather than held: a stream is
  /// compared as its whole text and shown as one line of it.
  json::Value Shown = nullptr;
};

/// How a row's sides are shown, once they are known to differ. Every row is
/// compared the same way -- as text -- and the shape only decides what is worth
/// putting in the document about a difference.
enum class Shape {
  /// As compared. The text is short and is what a reader wants.
  AsIs,

  /// Each run's summary of one capture, shortened against the others.
  Summary,

  /// Each run's output on one stream, reported as the line they part company on.
  Stream,
};

/// One thing compared across runs: what each run said about it, keyed by label.
struct Compared {
  std::map<std::string, Side> ByLabel;

  Shape How = Shape::AsIs;

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
    const std::string &First = ByLabel.begin()->second.Key;
    return all_of(ByLabel, [&](const auto &Entry) {
      return Entry.second.Key == First;
    });
  }
};

/// Everything compared, in the order it is reported.
class Table {
public:
  /// A row compared and shown as the same text.
  void Add(StringRef Name, StringRef Label, std::string Text) {
    Side &S = Ensure(Name).ByLabel[Label.str()];
    S.Key = Text;
    S.Shown = std::move(Text);
  }

  /// A row holding each run's summary of one capture: compared by its whole
  /// serialization, and shown as the document it is.
  void AddSummary(StringRef Name, StringRef Label, const json::Value &Summary) {
    Compared &Row = Ensure(Name);
    Row.How = Shape::Summary;
    Row.ByLabel[Label.str()] = Side{Render(Summary), Summary};
  }

  /// A row holding what each run wrote on one stream: compared as the whole text,
  /// so that identical output collapses to one name, and shown as the line they
  /// part company on.
  void AddStream(StringRef Name, StringRef Label, std::string Text) {
    Compared &Row = Ensure(Name);
    Row.How = Shape::Stream;
    Row.ByLabel[Label.str()].Key = std::move(Text);
  }

  /// Names in the order they were added, so that a report follows the plan rather
  /// than the alphabet.
  ArrayRef<std::string> Names() const { return m_order; }
  const Compared &Row(StringRef Name) const { return m_rows.at(Name.str()); }

private:
  Compared &Ensure(StringRef Name) {
    if (!m_index.count(Name.str())) {
      m_index[Name.str()] = m_order.size();
      m_order.push_back(Name.str());
    }
    return m_rows[Name.str()];
  }

  std::vector<std::string> m_order;
  std::map<std::string, size_t> m_index;
  std::map<std::string, Compared> m_rows;
};

/// One run's histogram of one capture, read out of its summary.
///
/// A histogram is keyed by the value where every value is a scalar and is a list
/// of value-and-count pairs where any of them is a document, so an entry is
/// identified here by the value as text -- which is the same thing either way --
/// and re-emitted in whichever shape the run used.
struct Histogram {
  bool Listed = false;
  std::map<std::string, std::string> CountOf;
  std::map<std::string, json::Value> EntryOf;
};

std::optional<Histogram> ReadHistogram(const json::Object &Summary) {
  const json::Value *Values = Summary.get("values");
  if (!Values)
    return std::nullopt;

  Histogram H;
  if (const json::Object *Keyed = Values->getAsObject()) {
    for (const auto &[Value, Count] : *Keyed) {
      H.CountOf[Value.str()] = Render(Count);
      H.EntryOf.insert_or_assign(Value.str(), Count);
    }
    return H;
  }
  if (const json::Array *Listed = Values->getAsArray()) {
    H.Listed = true;
    for (const json::Value &Entry : *Listed) {
      const json::Object *Pair = Entry.getAsObject();
      const json::Value *Value = Pair ? Pair->get("value") : nullptr;
      if (!Value)
        continue;
      const std::string Key = Render(*Value);
      const json::Value *Count = Pair->get("count");
      H.CountOf[Key] = Count ? Render(*Count) : std::string();
      H.EntryOf.insert_or_assign(Key, Entry);
    }
    return H;
  }
  return std::nullopt;
}

/// The names of the summary fields the runs do not agree on.
std::set<std::string> DisagreedFields(const std::map<std::string, Side> &ByLabel) {
  std::map<std::string, std::string> First;
  std::set<std::string> Out;
  for (const auto &[Label, S] : ByLabel) {
    const json::Object *Summary = S.Shown.getAsObject();
    if (!Summary)
      continue;
    for (const auto &[Field, Value] : *Summary) {
      auto [Seen, Fresh] = First.try_emplace(Field.str(), Render(Value));
      if (!Fresh && Seen->second != Render(Value))
        Out.insert(Field.str());
    }
    // A field one run has and another does not is a disagreement about it.
    for (const auto &[Field, Value] : First)
      if (!Summary->get(Field))
        Out.insert(Field);
  }
  return Out;
}

/// Each run's summary of one capture, cut down to what they disagree on.
///
/// A summary is bounded on its own, but two of them side by side in a document
/// that exists to be short is a different budget: the compare-drift pair was two
/// 260-char histograms of twenty-eight values whose real difference was one of
/// them. What differs is the entries whose count is not the same in every run --
/// the rest is the part of the run they have in common, which nobody asked for
/// twice. `distinct` and `values_elided` stay, because they are what makes a short
/// histogram read as a selection out of a longer one.
///
/// Transitions and outliers are dropped, since they are derived from the values
/// and a difference in them that the values do not already carry is reported by
/// name below rather than in full.
json::Object ShortenSummaries(const std::map<std::string, Side> &ByLabel) {
  std::map<std::string, Histogram> Histograms;
  for (const auto &[Label, S] : ByLabel)
    if (const json::Object *Summary = S.Shown.getAsObject())
      if (std::optional<Histogram> H = ReadHistogram(*Summary))
        Histograms.emplace(Label, std::move(*H));

  // Values held a different number of times by any two runs, which a value a run
  // never held at all is one of. Each run bounds its own histogram, so a value
  // another run kept and this one elided is reported here as a difference; the
  // `values_elided` beside it is what says the list is a selection.
  std::set<std::string> Differs;
  for (const auto &[Label, H] : Histograms)
    for (const auto &[Value, Count] : H.CountOf)
      for (const auto &[Other, OtherH] : Histograms) {
        auto Found = OtherH.CountOf.find(Value);
        if (Found == OtherH.CountOf.end() || Found->second != Count)
          Differs.insert(Value);
      }

  json::Object Out;
  for (const auto &[Label, S] : ByLabel) {
    auto Found = Histograms.find(Label);
    if (Found == Histograms.end()) {
      // Nothing to shorten. A capture that never varied renders as one word and a
      // single value renders as itself, which is already the shortest form there
      // is.
      Out[Label] = Bounded(S.Shown);
      continue;
    }

    const json::Object &Summary = *S.Shown.getAsObject();
    const Histogram &H = Found->second;
    json::Object Short;
    if (const json::Value *Distinct = Summary.get("distinct"))
      Short["distinct"] = *Distinct;

    json::Object Keyed;
    json::Array Listed;
    for (const std::string &Value : Differs) {
      auto Entry = H.EntryOf.find(Value);
      if (Entry == H.EntryOf.end())
        continue;
      if (H.Listed)
        Listed.push_back(Bounded(Entry->second));
      else
        Keyed[Truncate(Value)] = Bounded(Entry->second);
    }
    if (!Listed.empty())
      Short["values"] = std::move(Listed);
    else if (!Keyed.empty())
      Short["values"] = std::move(Keyed);
    if (const json::Value *Elided = Summary.get("values_elided"))
      Short["values_elided"] = *Elided;
    Out[Label] = std::move(Short);
  }

  // A row claiming a difference has to show one. Where the histograms agree and
  // the summaries differ in something derived from them -- the order the values
  // were taken in, which values were rare -- what is left above is the same
  // object twice, so the fields they actually disagree on are named and shown.
  bool Indistinguishable = Out.size() > 1;
  std::string First;
  for (const auto &[Label, Shown] : Out) {
    const std::string Text = Render(Shown);
    if (First.empty())
      First = Text;
    else if (Text != First)
      Indistinguishable = false;
  }
  if (!Indistinguishable)
    return Out;

  const std::set<std::string> Disagreed = DisagreedFields(ByLabel);
  for (auto &[Label, Shown] : Out) {
    const json::Object *Summary = ByLabel.at(Label.str()).Shown.getAsObject();
    json::Object *Short = Shown.getAsObject();
    if (!Summary || !Short)
      continue;
    for (const std::string &Field : Disagreed)
      if (const json::Value *Value = Summary->get(Field))
        (*Short)[Field] = Bounded(*Value);
  }
  return Out;
}

/// What each run wrote on one stream, reduced to the line they part company on.
///
/// The streams themselves do not go in the document. A program's output is bounded
/// at 8 kB per stream and two of those would be most of a response that is charged
/// for its size on every later turn -- while for a miscompile the wrong answer is
/// usually one line of stdout, and a caller who has the line has the difference.
/// Identical output produces no entry here at all: the whole text is what the runs
/// are compared on, so it collapses to one name under `agreed`.
json::Object ShortenStreams(const std::map<std::string, Side> &ByLabel) {
  std::map<std::string, SmallVector<StringRef, 8>> Split;
  for (const auto &[Label, S] : ByLabel)
    Split[Label] = Lines(S.Key);

  // The earliest line at which any run parted company with the first of them,
  // which for runs whose shared lines all agree is the first line the shorter one
  // does not have.
  const SmallVector<StringRef, 8> &Baseline = Split.begin()->second;
  std::optional<size_t> At;
  for (const auto &[Label, These] : Split) {
    const size_t Shared = std::min(Baseline.size(), These.size());
    size_t I = 0;
    while (I < Shared && Baseline[I] == These[I])
      ++I;
    if (I == Shared && These.size() == Baseline.size())
      continue;
    At = At ? std::min(*At, I) : I;
  }

  // No line differs when only one run wrote anything at all, and the difference
  // is then the whole of what it wrote against the other's silence.
  json::Object Out{{"first_differing_line", static_cast<int64_t>(At.value_or(0) + 1)}};
  for (const auto &[Label, These] : Split)
    Out[Label] = At.value_or(0) < These.size()
                     ? Truncate(These[At.value_or(0)].str(), MaxComparedOutputChars)
                     : std::string();
  return Out;
}

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

/// One capture that could not be read, as a comparison reports it.
///
/// A capture that resolved in one run and not in another is what the comparison
/// calls the commonest difference there is, and it appeared nowhere: the caller saw
/// an escaped `{"reason":"use of undeclared identifier 'i'","unavailable":"error"}`
/// inside `saw`, and only if that hit happened to be the divergent one.
///
/// The candidate names go, because two runs' lists of what was in scope instead are
/// the same list twice, and the detail is kept to its first line -- what the
/// compiler objected to, without the source excerpt and the caret under an
/// expression the caller wrote and already has.
json::Object DescribeFailure(const CaptureFailureReport &Failure) {
  json::Object O{{"observation", Failure.Label},
                 {"capture", Failure.Expr},
                 {"reason", ToString(Failure.Kind)}};

  // A condition is not a capture: it decides whether a hit is recorded at all, so
  // one that cannot be evaluated leaves an observation looking like code that never
  // ran.
  if (Failure.IsCondition)
    O["field"] = "when";
  if (!Failure.Reason.empty())
    O["detail"] =
        Truncate(StringRef(Failure.Reason).split('\n').first.str(), MaxComparedOutputChars);
  if (!Failure.FixedExpr.empty()) {
    O["fixed_as"] = Failure.FixedExpr;
    // A fixit that was suggested and failed as well is not a fix, and a caller who
    // read it as one would adopt a spelling that never resolved.
    O["fix_applied"] = Failure.FixApplied;
  }
  if (Failure.Disabled)
    O["disabled"] = true;
  return O;
}

/// The labels of \p In as an array, or nothing where every run that answered is in
/// it: a fault the whole set of runs hit is a fault in the request, and naming all
/// of them says only that.
std::optional<json::Array> Only(ArrayRef<std::string> In, size_t Answered) {
  if (In.size() == Answered)
    return std::nullopt;
  json::Array Out;
  for (const std::string &Label : In)
    Out.push_back(Label);
  return Out;
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

    // What the program itself said. For a miscompile the wrong answer usually *is*
    // stdout, and it was collected on every compared run and then dropped on the
    // floor: nothing here read it, so a pair of binaries that print different
    // results reported the difference only if a tracepoint happened to catch it.
    // Compared as the whole text, so two runs that printed the same thing cost one
    // name under `agreed` rather than two copies of their output.
    if (!R.Output.Out.empty())
      Compare.AddStream("stdout", Run.Label,
                        WithoutCarriageReturns(R.Output.Out));
    if (!R.Output.Err.empty())
      Compare.AddStream("stderr", Run.Label,
                        WithoutCarriageReturns(R.Output.Err));

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
            Compare.AddSummary(Label.str() + "." + Capture.str(), Run.Label,
                               Summary);
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
    switch (Row.How) {
    case Shape::AsIs:
      for (const auto &[Label, Answer] : Row.ByLabel)
        Sides[Label] = Bounded(Answer.Shown);
      break;
    case Shape::Summary:
      Sides = ShortenSummaries(Row.ByLabel);
      break;
    case Shape::Stream:
      Sides = ShortenStreams(Row.ByLabel);
      break;
    }
    Diverged[Name] = std::move(Sides);
  }

  // The hit at which the runs stopped agreeing, which for a miscompile or a hang
  // is the answer: everything before it is the same computation and everything
  // after is a consequence.
  //
  // Measured against the first run that produced a result rather than against the
  // last run. Comparing the first with the last skipped every run in between, so a
  // three-run plan whose middle run was the wrong one reported no divergent hit at
  // all and left the wrong run visible as one changed histogram bucket; and a first
  // run that could not be launched suppressed the whole field rather than yielding
  // the baseline to the run behind it.
  json::Object FirstDiverged;
  const ComparedRun *Baseline = nullptr;
  size_t BaselineAt = 0;
  for (const auto &[At, Run] : enumerate(Runs))
    if (Run.Result) {
      Baseline = &Run;
      BaselineAt = At;
      break;
    }

  for (const std::string &Label : Labels) {
    if (!Baseline)
      break;
    const ObservationReport *Base = FindObservation(*Baseline->Result, Label);
    if (!Base || Base->HitTuples.empty())
      continue;

    // The runs after the baseline with hits of this observation to compare.
    SmallVector<std::pair<const ComparedRun *, const ObservationReport *>, 4>
        Others;
    for (const ComparedRun &Run : Runs.drop_front(BaselineAt + 1)) {
      if (!Run.Result)
        continue;
      const ObservationReport *Report = FindObservation(*Run.Result, Label);
      if (Report && !Report->HitTuples.empty())
        Others.emplace_back(&Run, Report);
    }
    if (Others.empty())
      continue;

    // The earliest hit any run disagreed with the baseline at. One entry per
    // observation, because the question is where the plan first came apart and not
    // where each run did.
    std::optional<size_t> Earliest;
    for (const auto &[Run, Report] : Others)
      if (std::optional<size_t> At =
              FirstDifference(Base->HitTuples, Report->HitTuples))
        Earliest = Earliest ? std::min(*Earliest, *At) : *At;

    json::Object Entry;
    if (Earliest) {
      Entry["hit"] = static_cast<int64_t>(*Earliest + 1);
      json::Object Saw;
      Saw[Baseline->Label] =
          DescribeTuple(Base->HitTuples[*Earliest], Base->Captures);
      // Only the runs that differ there. A run whose own divergence comes later
      // agrees with the baseline at this hit, and listing it would read as
      // three-way disagreement about a hit two of them saw the same way.
      for (const auto &[Run, Report] : Others)
        if (Report->HitTuples.size() > *Earliest &&
            Report->HitTuples[*Earliest] != Base->HitTuples[*Earliest])
          Saw[Run->Label] =
              DescribeTuple(Report->HitTuples[*Earliest], Report->Captures);
      Entry["saw"] = std::move(Saw);
    } else {
      // A prefix of the others is not a disagreement about any hit: one run just
      // kept going. Reported as the count, because that is the difference.
      size_t Shortest = Base->HitTuples.size();
      bool Dropped = Base->HitTuplesDropped != 0;
      bool SameLength = true;
      for (const auto &[Run, Report] : Others) {
        Shortest = std::min(Shortest, Report->HitTuples.size());
        Dropped |= Report->HitTuplesDropped != 0;
        SameLength &= Report->HitTuples.size() == Base->HitTuples.size();
      }
      if (SameLength && !Dropped)
        continue;
      Entry["identical_through"] = static_cast<int64_t>(Shortest);
      if (Dropped)
        Entry["beyond_that"] =
            "not compared; more hits were taken than are kept for comparison";
    }
    FirstDiverged[Label] = std::move(Entry);
  }

  // What could not be read, and what went wrong that no observation owns. Both were
  // dropped in comparison mode, so a capture that resolved in one run and not in
  // another -- the commonest difference there is -- reached the caller only as an
  // escaped error inside `saw`, and only if that hit happened to be the divergent
  // one. Reported once each at top level, deduplicated across the runs, because two
  // runs failing at the same name is one fault in the request and not two.
  std::vector<std::string> FailureOrder;
  std::map<std::string, json::Object> FailureByKey;
  std::map<std::string, std::vector<std::string>> FailureIn;
  std::vector<std::string> NoteOrder;
  std::map<std::string, std::vector<std::string>> NoteIn;
  for (const ComparedRun &Run : Runs) {
    if (!Run.Result)
      continue;
    for (const CaptureFailureReport &Failure : Run.Result->CaptureFailures) {
      json::Object Described = DescribeFailure(Failure);
      std::string Key = Render(json::Object(Described));
      if (FailureByKey.try_emplace(Key, std::move(Described)).second)
        FailureOrder.push_back(Key);
      // Once per run: a run that reported one failure twice must not count twice
      // towards "every run said this", which is what decides whether the labels
      // appear at all.
      std::vector<std::string> &In = FailureIn[std::move(Key)];
      if (In.empty() || In.back() != Run.Label)
        In.push_back(Run.Label);
    }
    for (const std::string &Note : Run.Result->Notes) {
      if (!NoteIn.count(Note))
        NoteOrder.push_back(Note);
      std::vector<std::string> &In = NoteIn[Note];
      if (In.empty() || In.back() != Run.Label)
        In.push_back(Run.Label);
    }
  }

  json::Array Failures;
  for (const std::string &Key : FailureOrder) {
    json::Object Entry = std::move(FailureByKey.at(Key));
    if (std::optional<json::Array> In = Only(FailureIn.at(Key), Answered))
      Entry["in"] = std::move(*In);
    Failures.push_back(std::move(Entry));
  }

  // A note every run made is the string it is, as it is in a single run's report. One
  // only some runs made carries the labels, which is the whole of why it is here.
  json::Array Notes;
  for (const std::string &Note : NoteOrder) {
    std::optional<json::Array> In = Only(NoteIn.at(Note), Answered);
    if (!In)
      Notes.push_back(Note);
    else
      Notes.push_back(json::Object{{"note", Note}, {"in", std::move(*In)}});
  }

  json::Object Out{{"runs", std::move(Rows)}};
  if (!Diverged.empty())
    Out["diverged"] = std::move(Diverged);
  if (!FirstDiverged.empty())
    Out["first_divergent_hit"] = std::move(FirstDiverged);
  if (!Failures.empty())
    Out["capture_failures"] = std::move(Failures);
  if (!Notes.empty())
    Out["notes"] = std::move(Notes);
  // Names alone. What a caller asks a comparison is whether anything else moved,
  // and the answer is a list of what did not rather than the values that did not.
  //
  // Not said at all when only one run answered. Every row of a single run agrees
  // with itself, so the list would be as long as the run's own report while
  // meaning nothing -- and a caller reading it would take it for evidence that
  // the change under comparison moved nothing, when the other side of the
  // comparison never ran at all.
  if (Answered > 1 && !Agreed.empty())
    Out["agreed"] = std::move(Agreed);
  return Out;
}
