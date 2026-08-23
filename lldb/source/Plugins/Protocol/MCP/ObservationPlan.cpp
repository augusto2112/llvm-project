//===- ObservationPlan.cpp ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ObservationPlan.h"
#include "lldb/Breakpoint/Breakpoint.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleList.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/Utility/RegularExpression.h"
#include "lldb/lldb-enumerations.h"
#include "lldb/lldb-forward.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace lldb_private;
using namespace lldb_private::mcp;
using namespace llvm;

namespace {

/// The fields a plan may carry. Anything else is rejected, so this list is
/// also what an error message offers as the alternative to a typo.
constexpr StringRef PlanFields[] = {
    "program",
    "args",
    "env",
    "cwd",
    "stdin",
    "capture_inferior_output",
    "observe",
    "timeout_seconds",
    "no_progress_seconds",
};

constexpr StringRef ObservationFields[] = {
    "label",         "at",         "on",
    "capture",       "when",       "called_from",
    "enabled_after", "skip_first", "only_hit",
    "emit",          "backtrace",  "depth",
};

std::string QuotedList(ArrayRef<StringRef> Names) {
  std::string Out;
  raw_string_ostream OS(Out);
  ListSeparator LS;
  for (StringRef Name : Names)
    OS << LS << '"' << Name << '"';
  return Out;
}

Error UnrecognizedFields(const json::Object &Obj, ArrayRef<StringRef> Known,
                         StringRef Where) {
  SmallVector<StringRef, 4> Unknown;
  for (const auto &KV : Obj)
    if (!is_contained(Known, StringRef(KV.first)))
      Unknown.push_back(KV.first);
  if (Unknown.empty())
    return Error::success();

  // Object iteration order is unspecified, so the message is sorted to keep it
  // reproducible for the same input.
  llvm::sort(Unknown);
  return createStringError(
      formatv("{0}: unrecognized field{1} {2}. Accepted fields are {3}. An "
              "unrecognized field is an error rather than a default, because a "
              "field that is ignored is indistinguishable in the result from "
              "one that was honoured.",
              Where, Unknown.size() == 1 ? "" : "s", QuotedList(Unknown),
              QuotedList(Known))
          .str());
}

/// Reads a string field. Absent yields nullopt; present with any other type is
/// an error, since a value of the wrong shape is a mistake rather than a
/// request for the default.
Expected<std::optional<std::string>>
GetString(const json::Object &Obj, StringRef Field, StringRef Where) {
  const json::Value *V = Obj.get(Field);
  if (!V)
    return std::nullopt;
  std::optional<StringRef> S = V->getAsString();
  if (!S)
    return createStringError(
        formatv("{0}: \"{1}\" must be a string.", Where, Field).str());
  return S->str();
}

/// Reads a string field that must name something if it is present at all.
Expected<std::optional<std::string>>
GetNonEmptyString(const json::Object &Obj, StringRef Field, StringRef Where) {
  Expected<std::optional<std::string>> S = GetString(Obj, Field, Where);
  if (!S)
    return S.takeError();
  if (*S && (*S)->empty())
    return createStringError(
        formatv("{0}: \"{1}\" is empty. Leave it out to get the default rather "
                "than passing an empty string, which names nothing.",
                Where, Field)
            .str());
  return S;
}

Expected<std::optional<bool>> GetBool(const json::Object &Obj, StringRef Field,
                                      StringRef Where) {
  const json::Value *V = Obj.get(Field);
  if (!V)
    return std::nullopt;
  std::optional<bool> B = V->getAsBoolean();
  if (!B)
    return createStringError(
        formatv("{0}: \"{1}\" must be true or false.", Where, Field).str());
  return B;
}

Expected<std::optional<uint32_t>> GetUInt(const json::Object &Obj,
                                          StringRef Field, StringRef Where) {
  const json::Value *V = Obj.get(Field);
  if (!V)
    return std::nullopt;
  std::optional<int64_t> N = V->getAsInteger();
  if (!N)
    return createStringError(
        formatv("{0}: \"{1}\" must be a whole number.", Where, Field).str());
  if (*N < 0 || *N > std::numeric_limits<uint32_t>::max())
    return createStringError(
        formatv("{0}: \"{1}\" is {2}, which is out of range; it must be "
                "between 0 and {3}.",
                Where, Field, *N, std::numeric_limits<uint32_t>::max())
            .str());
  return static_cast<uint32_t>(*N);
}

Expected<std::vector<std::string>>
GetStringArray(const json::Object &Obj, StringRef Field, StringRef Where) {
  std::vector<std::string> Out;
  const json::Value *V = Obj.get(Field);
  if (!V)
    return Out;
  const json::Array *A = V->getAsArray();
  if (!A)
    return createStringError(
        formatv("{0}: \"{1}\" must be an array of strings.", Where, Field)
            .str());
  Out.reserve(A->size());
  for (const json::Value &Element : *A) {
    std::optional<StringRef> S = Element.getAsString();
    if (!S)
      return createStringError(
          formatv("{0}: every entry in \"{1}\" must be a string.", Where, Field)
              .str());
    Out.push_back(S->str());
  }
  return Out;
}

Expected<StringMap<std::string>> GetEnv(const json::Object &Obj,
                                        StringRef Where) {
  StringMap<std::string> Out;
  const json::Value *V = Obj.get("env");
  if (!V)
    return Out;
  const json::Object *O = V->getAsObject();
  if (!O)
    return createStringError(
        formatv("{0}: \"env\" must be an object mapping variable names to "
                "string values.",
                Where)
            .str());
  for (const auto &KV : *O) {
    std::optional<StringRef> S = KV.second.getAsString();
    if (!S)
      return createStringError(
          formatv("{0}: the \"env\" value for \"{1}\" must be a string; a "
                  "number or boolean is not converted to one.",
                  Where, StringRef(KV.first))
              .str());
    Out[StringRef(KV.first)] = S->str();
  }
  return Out;
}

/// Whether a location is written as a raw address.
bool IsRawAddress(StringRef Location) {
  if (!Location.consume_front("0x") && !Location.consume_front("0X"))
    return false;
  return !Location.empty() && all_of(Location, isHexDigit);
}

Error RawAddressError(StringRef Field, StringRef Location, StringRef Where) {
  return createStringError(
      formatv("{0}: \"{1}\" is the address {2}, which cannot be observed. An "
              "address is only meaningful inside the run that produced it: "
              "address space layout randomization relocates the image and "
              "allocation order differs, so in the next run the same number "
              "names a different instruction, or none at all. Name the "
              "function instead.",
              Where, Field, Location)
          .str());
}

/// Splits a `file.cpp:1189` location. A trailing colon-and-digits is what
/// marks the source-location form, which is why a qualified function name like
/// `Foo::bar` is not mistaken for one.
bool ParseFileLine(StringRef Location, StringRef &File, uint32_t &Line) {
  size_t Colon = Location.rfind(':');
  if (Colon == StringRef::npos || Colon == 0)
    return false;
  StringRef LineText = Location.substr(Colon + 1);
  if (LineText.empty() || !all_of(LineText, isDigit))
    return false;
  if (LineText.getAsInteger(10, Line))
    return false;
  File = Location.substr(0, Colon);
  return !File.empty();
}

Expected<Observation> ParseObservation(const json::Value &V, size_t Index) {
  const json::Object *Obj = V.getAsObject();
  if (!Obj)
    return createStringError(
        formatv("observation #{0}: every entry in \"observe\" must be an "
                "object.",
                Index + 1)
            .str());

  // The label is read first so that every later message can name the
  // observation the way the caller wrote it.
  std::string Where = formatv("observation #{0}", Index + 1).str();
  Expected<std::optional<std::string>> Label =
      GetNonEmptyString(*Obj, "label", Where);
  if (!Label)
    return Label.takeError();
  if (*Label)
    Where = formatv("observation \"{0}\"", **Label).str();

  if (Error E = UnrecognizedFields(*Obj, ObservationFields, Where))
    return std::move(E);

  Observation Obs;

  Expected<std::optional<std::string>> At = GetString(*Obj, "at", Where);
  if (!At)
    return At.takeError();
  if (!*At || (*At)->empty())
    return createStringError(
        formatv("{0}: \"at\" is required and names the function to observe.",
                Where)
            .str());
  if (IsRawAddress(**At))
    return RawAddressError("at", **At, Where);

  StringRef File;
  uint32_t Line = 0;
  if (ParseFileLine(**At, File, Line)) {
    if (Line == 0)
      return createStringError(
          formatv("{0}: \"at\" is \"{1}\", but line numbers count from 1.",
                  Where, **At)
              .str());
    Obs.At = File.str();
    Obs.AtLine = Line;
  } else {
    Obs.At = std::move(**At);
  }
  Obs.Label = *Label
                  ? std::move(**Label)
                  : (Obs.AtLine ? formatv("{0}:{1}", Obs.At, *Obs.AtLine).str()
                                : Obs.At);

  Expected<std::optional<std::string>> On =
      GetNonEmptyString(*Obj, "on", Where);
  if (!On)
    return On.takeError();
  if (*On) {
    if (**On == "return")
      Obs.OnReturn = true;
    else if (**On != "entry")
      return createStringError(
          formatv("{0}: \"on\" is \"{1}\"; it must be \"entry\" or \"return\".",
                  Where, **On)
              .str());
  }

  Expected<std::vector<std::string>> Capture =
      GetStringArray(*Obj, "capture", Where);
  if (!Capture)
    return Capture.takeError();
  for (StringRef Expr : *Capture)
    if (Expr.trim().empty())
      return createStringError(
          formatv("{0}: \"capture\" contains an empty expression. Drop the "
                  "entry, or drop \"capture\" entirely for a tracepoint that "
                  "records only hit counts.",
                  Where)
              .str());
  Obs.Capture = std::move(*Capture);

  Expected<std::optional<std::string>> When =
      GetNonEmptyString(*Obj, "when", Where);
  if (!When)
    return When.takeError();
  Obs.WhenExpr = std::move(*When);

  Expected<std::optional<std::string>> CalledFrom =
      GetNonEmptyString(*Obj, "called_from", Where);
  if (!CalledFrom)
    return CalledFrom.takeError();
  if (*CalledFrom && IsRawAddress(**CalledFrom))
    return RawAddressError("called_from", **CalledFrom, Where);
  Obs.CalledFrom = std::move(*CalledFrom);

  Expected<std::optional<std::string>> EnabledAfter =
      GetNonEmptyString(*Obj, "enabled_after", Where);
  if (!EnabledAfter)
    return EnabledAfter.takeError();
  Obs.EnabledAfter = std::move(*EnabledAfter);

  Expected<std::optional<uint32_t>> SkipFirst =
      GetUInt(*Obj, "skip_first", Where);
  if (!SkipFirst)
    return SkipFirst.takeError();
  Obs.SkipFirst = SkipFirst->value_or(Obs.SkipFirst);

  Expected<std::optional<uint32_t>> OnlyHit = GetUInt(*Obj, "only_hit", Where);
  if (!OnlyHit)
    return OnlyHit.takeError();
  if (*OnlyHit && **OnlyHit == 0)
    return createStringError(
        formatv("{0}: \"only_hit\" is 0, which selects no hit at all; hits are "
                "counted from 1.",
                Where)
            .str());
  Obs.OnlyHit = *OnlyHit;

  Expected<std::optional<std::string>> Emit =
      GetNonEmptyString(*Obj, "emit", Where);
  if (!Emit)
    return Emit.takeError();
  if (*Emit) {
    if (**Emit == "every_hit")
      Obs.Emit = EmitMode::EveryHit;
    else if (**Emit == "on_change")
      Obs.Emit = EmitMode::OnChange;
    else if (**Emit == "first_and_last")
      Obs.Emit = EmitMode::FirstAndLast;
    else
      return createStringError(
          formatv("{0}: \"emit\" is \"{1}\"; it must be \"every_hit\", "
                  "\"on_change\" or \"first_and_last\".",
                  Where, **Emit)
              .str());
  }

  Expected<std::optional<uint32_t>> Backtrace =
      GetUInt(*Obj, "backtrace", Where);
  if (!Backtrace)
    return Backtrace.takeError();
  Obs.Backtrace = Backtrace->value_or(Obs.Backtrace);

  Expected<std::optional<uint32_t>> Depth = GetUInt(*Obj, "depth", Where);
  if (!Depth)
    return Depth.takeError();
  Obs.Depth = Depth->value_or(Obs.Depth);

  return Obs;
}

/// Rejects labels that are used twice, and gates that can never open.
Error CheckLabels(ArrayRef<Observation> Observations) {
  StringMap<size_t> ByLabel;
  for (size_t I = 0; I < Observations.size(); ++I) {
    const Observation &Obs = Observations[I];
    if (!ByLabel.try_emplace(Obs.Label, I).second)
      return createStringError(
          formatv("two observations share the label \"{0}\". A label names one "
                  "observation in the report and in \"enabled_after\", so it "
                  "has to be unique; an observation with no label takes the "
                  "name of the function it observes.",
                  Obs.Label)
              .str());
  }

  SmallVector<StringRef, 8> Labels;
  for (const Observation &Obs : Observations)
    Labels.push_back(Obs.Label);

  for (const Observation &Obs : Observations) {
    if (!Obs.EnabledAfter)
      continue;
    if (!ByLabel.contains(*Obs.EnabledAfter))
      return createStringError(
          formatv("observation \"{0}\": \"enabled_after\" names \"{1}\", which "
                  "is not the label of any observation in this plan. The "
                  "labels in this plan are {2}. A gate on a label that does "
                  "not exist would leave the observation disabled for the "
                  "whole run.",
                  Obs.Label, *Obs.EnabledAfter, QuotedList(Labels))
              .str());
  }

  // A gate chain that loops never opens: each observation in the loop waits for
  // another one that is itself waiting.
  for (size_t I = 0; I < Observations.size(); ++I) {
    size_t Current = I;
    SmallVector<StringRef, 4> Chain;
    // A chain reaches each observation at most once before it repeats, so this
    // many steps is enough to leave a loop that does not run back through I.
    for (size_t Step = 0; Step <= Observations.size(); ++Step) {
      const Observation &Obs = Observations[Current];
      if (!Obs.EnabledAfter)
        break;
      Chain.push_back(Obs.Label);
      Current = ByLabel.find(*Obs.EnabledAfter)->second;
      if (Current != I)
        continue;

      Chain.push_back(Observations[Current].Label);
      std::string Cycle;
      raw_string_ostream OS(Cycle);
      ListSeparator LS(" -> ");
      for (StringRef Label : Chain)
        OS << LS << '"' << Label << '"';
      return createStringError(
          formatv("\"enabled_after\" forms a cycle: {0}. Every observation in "
                  "the cycle waits for another one that is itself waiting, so "
                  "none of them is ever enabled.",
                  Cycle)
              .str());
    }
  }

  return Error::success();
}

/// Every function name in the modules loaded into \p Tgt, sorted and
/// deduplicated. Worth building only once a name has failed to resolve, since
/// at that point the whole set has to be ranked: a misspelling shares no
/// dependable substring with the name that was meant.
std::vector<std::string> CollectFunctionNames(Target &Tgt) {
  ModuleFunctionSearchOptions Options;
  Options.include_symbols = true;
  Options.include_inlines = false;

  SymbolContextList Found;
  RegularExpression AnyName(".");
  Tgt.GetImages().FindFunctions(AnyName, Options, Found);

  std::vector<std::string> Names;
  Names.reserve(Found.GetSize());
  for (const SymbolContext &SC : Found)
    if (ConstString Name = SC.GetFunctionName())
      Names.push_back(Name.GetStringRef().str());

  llvm::sort(Names);
  Names.erase(std::unique(Names.begin(), Names.end()), Names.end());
  return Names;
}

/// The names closest to \p Wanted, nearest first, at most \p Limit of them.
std::vector<StringRef> NearestNames(StringRef Wanted,
                                    ArrayRef<std::string> Names, size_t Limit) {
  // Tolerance grows with length: one slip in a short name makes a different
  // name, while a long name stays recognisable through a couple of them.
  const unsigned Tolerance =
      std::max<unsigned>(2, static_cast<unsigned>(Wanted.size() / 3));

  SmallVector<std::pair<unsigned, StringRef>, 8> Ranked;
  for (StringRef Name : Names) {
    unsigned Distance =
        Wanted.edit_distance(Name, /*AllowReplacements=*/true, Tolerance);
    // An exact match is not a suggestion: repeating the name back says nothing
    // about why it did not resolve.
    if (Distance != 0 && Distance <= Tolerance)
      Ranked.emplace_back(Distance, Name);
  }

  // Names arrive sorted, so a stable sort leaves equally close names in
  // alphabetical order.
  llvm::stable_sort(Ranked, [](const std::pair<unsigned, StringRef> &A,
                               const std::pair<unsigned, StringRef> &B) {
    return A.first < B.first;
  });

  std::vector<StringRef> Nearest;
  for (const std::pair<unsigned, StringRef> &Candidate : Ranked) {
    if (Nearest.size() >= Limit)
      break;
    Nearest.push_back(Candidate.second);
  }
  return Nearest;
}

std::string DescribeUnresolved(StringRef At, ArrayRef<std::string> Names) {
  std::vector<StringRef> Nearest = NearestNames(At, Names, /*Limit=*/3);

  std::string Message;
  raw_string_ostream OS(Message);
  OS << formatv("no code matched \"{0}\", so this observation cannot fire.",
                At);
  if (Nearest.empty())
    OS << " No similar name is present in the loaded modules either. A "
          "function in a library that has not been loaded yet resolves when "
          "the library loads; otherwise the name is not in this program.";
  else
    OS << formatv(" Did you mean {0}? Names are matched exactly, so a name "
                  "that is close is still a name that never fires.",
                  QuotedList(Nearest));
  return Message;
}

} // namespace

StringRef lldb_private::mcp::ToString(EmitMode Mode) {
  switch (Mode) {
  case EmitMode::EveryHit:
    return "every_hit";
  case EmitMode::OnChange:
    return "on_change";
  case EmitMode::FirstAndLast:
    return "first_and_last";
  }
  llvm_unreachable("unhandled EmitMode");
}

Expected<ObservationPlan>
lldb_private::mcp::ParseObservationPlan(const json::Value &Plan) {
  const json::Object *Obj = Plan.getAsObject();
  if (!Obj)
    return createStringError(
        "an observation plan must be a JSON object whose only required field "
        "is \"program\", the path to the program to run.");

  constexpr StringLiteral Where = "plan";
  if (Error E = UnrecognizedFields(*Obj, PlanFields, Where))
    return std::move(E);

  ObservationPlan Result;

  Expected<std::optional<std::string>> Program =
      GetString(*Obj, "program", Where);
  if (!Program)
    return Program.takeError();
  if (!*Program || (*Program)->empty())
    return createStringError(
        "plan: \"program\" is required and is the path to the program to run.");
  Result.Program = std::move(**Program);

  Expected<std::vector<std::string>> Args = GetStringArray(*Obj, "args", Where);
  if (!Args)
    return Args.takeError();
  Result.Args = std::move(*Args);

  Expected<StringMap<std::string>> Env = GetEnv(*Obj, Where);
  if (!Env)
    return Env.takeError();
  Result.Env = std::move(*Env);

  Expected<std::optional<std::string>> Cwd =
      GetNonEmptyString(*Obj, "cwd", Where);
  if (!Cwd)
    return Cwd.takeError();
  Result.Cwd = std::move(*Cwd);

  Expected<std::optional<std::string>> Stdin =
      GetNonEmptyString(*Obj, "stdin", Where);
  if (!Stdin)
    return Stdin.takeError();
  Result.Stdin = std::move(*Stdin);

  Expected<std::optional<bool>> CaptureOutput =
      GetBool(*Obj, "capture_inferior_output", Where);
  if (!CaptureOutput)
    return CaptureOutput.takeError();
  Result.CaptureInferiorOutput =
      CaptureOutput->value_or(Result.CaptureInferiorOutput);

  Expected<std::optional<uint32_t>> Timeout =
      GetUInt(*Obj, "timeout_seconds", Where);
  if (!Timeout)
    return Timeout.takeError();
  if (*Timeout && **Timeout == 0)
    return createStringError(
        "plan: \"timeout_seconds\" is 0, which would end the run before the "
        "program starts; it must be at least 1.");
  Result.TimeoutSeconds = Timeout->value_or(Result.TimeoutSeconds);

  Expected<std::optional<uint32_t>> NoProgress =
      GetUInt(*Obj, "no_progress_seconds", Where);
  if (!NoProgress)
    return NoProgress.takeError();
  if (*NoProgress && **NoProgress >= Result.TimeoutSeconds)
    return createStringError(
        formatv("plan: \"no_progress_seconds\" is {0}, which is not shorter "
                "than \"timeout_seconds\" ({1}), so the run would end on the "
                "timeout before a stall could ever be reported. Lower it, or "
                "raise the timeout.",
                **NoProgress, Result.TimeoutSeconds)
            .str());
  Result.NoProgressSeconds = *NoProgress;

  if (const json::Value *Observe = Obj->get("observe")) {
    const json::Array *Array = Observe->getAsArray();
    if (!Array)
      return createStringError(
          "plan: \"observe\" must be an array of observations. Leave it out to "
          "run the program with no tracepoints and report only how it ended.");
    Result.Observations.reserve(Array->size());
    for (size_t I = 0; I < Array->size(); ++I) {
      Expected<Observation> Obs = ParseObservation((*Array)[I], I);
      if (!Obs)
        return Obs.takeError();
      Result.Observations.push_back(std::move(*Obs));
    }
  }

  if (Error E = CheckLabels(Result.Observations))
    return std::move(E);

  return Result;
}

std::vector<LocationResolution>
lldb_private::mcp::ResolveObservationLocations(ObservationPlan &Plan,
                                               Target &Tgt) {
  std::vector<LocationResolution> Resolutions;
  Resolutions.reserve(Plan.Observations.size());

  std::optional<std::vector<std::string>> Names;

  for (const Observation &Obs : Plan.Observations) {
    LocationResolution Resolution;

    // Both trigger points resolve to the function's entry. A return
    // observation is derived from the entry hit, which keeps it independent of
    // where a compiler chose to put the epilogue.
    Resolution.Breakpoint = Tgt.CreateBreakpoint(
        /*containingModules=*/nullptr, /*containingSourceFiles=*/nullptr,
        Obs.At.c_str(), lldb::eFunctionNameTypeAuto, lldb::eLanguageTypeUnknown,
        /*offset=*/0, /*offset_is_insn_count=*/false, eLazyBoolCalculate,
        /*internal=*/false, /*request_hardware=*/false);
    if (Resolution.Breakpoint)
      Resolution.ResolvedLocations =
          static_cast<uint32_t>(Resolution.Breakpoint->GetNumLocations());

    if (Resolution.ResolvedLocations == 0) {
      if (!Names)
        Names = CollectFunctionNames(Tgt);
      Resolution.Error = DescribeUnresolved(Obs.At, *Names);
    }

    Resolutions.push_back(std::move(Resolution));
  }

  return Resolutions;
}
