//===- ObservationPlan.cpp ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ObservationPlan.h"
#include "lldb/Breakpoint/Breakpoint.h"
#include "lldb/Breakpoint/BreakpointLocation.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleList.h"
#include "lldb/Core/Mangled.h"
#include "lldb/Symbol/LineEntry.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Target/FunctionPatch.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/Utility/FileSpec.h"
#include "lldb/Utility/RegularExpression.h"
#include "lldb/lldb-enumerations.h"
#include "lldb/lldb-forward.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
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
#include <chrono>
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
    "compare",
};

/// The fields one entry of `compare` may carry: a label and the parts of a plan
/// that may differ between runs. The tracepoints are deliberately not among them.
constexpr StringRef VariantFields[] = {
    "label", "program", "args", "env", "cwd", "stdin",
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

/// One value as the caller wrote it, for a message that has to say which value was
/// wrong rather than only which field.
///
/// Naming the field alone leaves the caller to find the value, and the mistakes
/// this answers are ones where the value is the whole of it: `"timeout_seconds":
/// "30"` is a number written as a string, and "must be a whole number" reads as a
/// contradiction until the quotes are visible. JSON's own rendering is what shows
/// them.
///
/// Truncated, because the value came from the caller and an error is not the place
/// to echo an arbitrary amount of it back. What identifies the mistake is the front
/// of it; a plan that put a whole document where a scalar belonged would otherwise
/// put that document in the message.
std::string AsWritten(const json::Value &V) {
  constexpr size_t MaxChars = 60;
  std::string Out;
  raw_string_ostream OS(Out);
  OS << V;
  if (Out.size() > MaxChars)
    return Out.substr(0, MaxChars) + "...";
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
        formatv("{0}: \"{1}\" must be a string, not {2}.", Where, Field,
                AsWritten(*V))
            .str());
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
        formatv("{0}: \"{1}\" must be true or false, not {2}.", Where, Field,
                AsWritten(*V))
            .str());
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
        formatv("{0}: \"{1}\" must be a whole number, not {2}.", Where, Field,
                AsWritten(*V))
            .str());
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
        formatv("{0}: \"{1}\" must be an array of strings, not {2}.", Where,
                Field, AsWritten(*V))
            .str());
  Out.reserve(A->size());
  for (size_t I = 0; I < A->size(); ++I) {
    std::optional<StringRef> S = (*A)[I].getAsString();
    if (!S)
      // Subscripted, because the caller has to find the one entry among the
      // several it wrote: a six-element `capture` otherwise leaves it guessing.
      return createStringError(
          formatv("{0}: \"{1}\"[{2}] is {3}; every entry must be a string.",
                  Where, Field, I, AsWritten((*A)[I]))
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
                "string values, not {1}.",
                Where, AsWritten(*V))
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
                "object, and this one is {1}.",
                Index + 1, AsWritten(V))
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
  // Subscripted for the same reason a wrongly typed entry is: with six captures
  // written, which one is empty is otherwise left to the caller to find.
  for (size_t I = 0; I < Capture->size(); ++I)
    if (StringRef((*Capture)[I]).trim().empty())
      return createStringError(
          formatv("{0}: \"capture\"[{1}] is empty. Drop the entry, or drop "
                  "\"capture\" entirely for a tracepoint that records only hit "
                  "counts.",
                  Where, I)
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

/// Names collected for a suggestion, and how long collecting them may take.
///
/// The time is a real bound and is reached: a spelling is tested by asking for the
/// breakpoint it would resolve to, measured at about 7 ms against a 238 MB debug
/// build, and a long name has a few thousand spellings one character away from it.
/// Two seconds buys the likeliest few hundred of them, is charged only where a
/// name has already failed to resolve, and is what the search it replaced spent
/// every 14 names -- that one cost 332 seconds and took them from the run.
constexpr size_t MaxSuggestionCandidates = 4096;
constexpr std::chrono::milliseconds MaxSuggestionTime{2000};

/// How far off a name may be and still be worth suggesting. Tolerance grows with
/// length: one slip in a short name makes a different name, while a long name
/// stays recognisable through a couple of them.
unsigned NameTolerance(StringRef Name) {
  return std::max<unsigned>(2, static_cast<unsigned>(Name.size() / 3));
}

/// The last component of \p At, which is the part a misspelling is in: a caller
/// writing "llvm::VectorCombine::foldBitcastShufle" has the scope right and the
/// name wrong, because the scope is what they copied.
StringRef BaseName(StringRef At) {
  const size_t Scope = At.rfind("::");
  return Scope == StringRef::npos ? At : At.drop_front(Scope + 2);
}

/// Names one edit away from \p At, found by looking each spelling up rather than
/// by searching the program for names resembling it.
///
/// Searching is the obvious way and it cannot be made cheap. A regex over every
/// function has to walk the debug information of every compile unit, since a
/// pattern cannot use the name index: measured on a 238 MB debug build of a
/// compiler, 332 seconds with the symbol table included and 99 without -- charged
/// to a caller who had made a typo, whose run then reported the program as having
/// hung inside the dynamic loader because the wall clock had gone on the
/// suggestion.
///
/// Looking up is the other direction. An exact name goes through the accelerated
/// index, which is a hash probe and is already warm from the failed resolution
/// that got us here, so the cost is one probe per spelling rather than one
/// comparison per function in the program. The spellings are the classic single
/// edits -- a character dropped, two transposed, one replaced, one inserted --
/// which is what a typo is.
///
/// Only the last component is varied, and each spelling is looked up unqualified.
/// A caller who has the scope wrong is answered anyway: "VectorCombine" that is
/// really in an anonymous namespace resolves by base name.
struct Suggestions {
  std::vector<std::string> Names;

  /// Whether the search stopped before it had tried every spelling. What the
  /// message may claim depends on it: "nothing is close" is a statement about the
  /// program, and a search that was cut short did not establish it.
  bool Partial = false;
};

Suggestions CollectCandidateNames(Target &Tgt, StringRef At) {
  const StringRef Wanted = BaseName(At);
  if (Wanted.size() < 2)
    return {};
  Suggestions Result;

  std::vector<std::string> &Names = Result.Names;
  const auto Started = std::chrono::steady_clock::now();
  bool &OutOfTime = Result.Partial;

  // Tested by asking for the breakpoint the observation would have asked for.
  // Looking the name up directly is cheaper and does not agree: a bare
  // `visitAdd` finds nothing through `FindFunctions` under any name-type mask
  // that also works for a free function, while the resolver finds the 27
  // locations of the method by that name. Agreeing with the resolver is also what
  // makes the suggestion worth printing -- every name suggested is one that would
  // have resolved, rather than one that merely exists somewhere in the program.
  auto Probe = [&](const std::string &Spelling) {
    if (OutOfTime || Names.size() >= MaxSuggestionCandidates)
      return;
    // Read per probe rather than around the whole loop: what has to be bounded is
    // the time a suggestion costs the run, and a program whose index is slower
    // than expected is exactly the case a fixed count would not catch.
    if (std::chrono::steady_clock::now() - Started > MaxSuggestionTime) {
      OutOfTime = true;
      return;
    }
    lldb::BreakpointSP BP = Tgt.CreateBreakpoint(
        /*containingModules=*/nullptr, /*containingSourceFiles=*/nullptr,
        Spelling.c_str(), lldb::eFunctionNameTypeAuto, lldb::eLanguageTypeUnknown,
        /*offset=*/0, /*offset_is_insn_count=*/false, eLazyBoolCalculate,
        /*internal=*/false, /*request_hardware=*/false);
    if (!BP)
      return;
    if (BP->GetNumLocations() != 0)
      Names.push_back(Spelling);
    // Internal breakpoints stay out of the caller's list, but not out of the
    // target's: a run that left two thousand of them behind would install every
    // one of them.
    Tgt.RemoveBreakpointByID(BP->GetID());
  };

  static constexpr StringLiteral Alphabet =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";

  // Ordered by class rather than by position, so that a budget spent before the
  // end has been spent on the likelier mistakes. A dropped character and two
  // transposed are a handful of spellings between them; replacing and inserting
  // are sixty-three each per position, and they are the classes a bound reaches.
  const std::string Base = Wanted.str();
  for (size_t I = 0; I < Base.size(); ++I) {
    std::string Dropped = Base;
    Dropped.erase(I, 1);
    Probe(Dropped);
  }
  for (size_t I = 0; I + 1 < Base.size(); ++I) {
    if (Base[I] == Base[I + 1])
      continue;
    std::string Swapped = Base;
    std::swap(Swapped[I], Swapped[I + 1]);
    Probe(Swapped);
  }
  for (size_t I = 0; I < Base.size(); ++I)
    for (char C : Alphabet)
      if (C != Base[I]) {
        std::string Replaced = Base;
        Replaced[I] = C;
        Probe(Replaced);
      }
  for (size_t I = 0; I <= Base.size(); ++I)
    for (char C : Alphabet) {
      std::string Inserted = Base;
      Inserted.insert(I, 1, C);
      Probe(Inserted);
    }

  llvm::sort(Names);
  Names.erase(std::unique(Names.begin(), Names.end()), Names.end());
  return Result;
}

std::string DescribeUnresolved(StringRef At, const Suggestions &Found) {
  std::vector<StringRef> Nearest = NearestNames(At, Found.Names, /*Limit=*/3);

  std::string Message;
  raw_string_ostream OS(Message);
  OS << formatv("no code matched \"{0}\", so this observation cannot fire.",
                At);
  if (Nearest.empty())
    // Precisely what was looked for, because a suggestion is drawn by trying the
    // spellings one character away from this one rather than by comparing against
    // every name in the program: a name two characters wrong gets no suggestion,
    // and saying "nothing similar exists" would be a claim about the program that
    // was never tested. Where the search ran out of time it is less than that
    // again, and says so.
    OS << (Found.Partial
               ? " The spellings one character away from it were being tried "
                 "and the search ran out of time, so there may be a near one it "
                 "did not reach."
               : " No name one character away from it exists in the loaded "
                 "modules either.")
       << " A function in a library that has not been loaded yet resolves when "
          "the library loads; otherwise check the spelling against the source, "
          "or name the enclosing file and line instead.";
  else
    OS << formatv(" Did you mean {0}? Names are matched exactly, so a name "
                  "that is close is still a name that never fires.",
                  QuotedList(Nearest));
  return Message;
}

} // namespace

std::vector<StringRef>
lldb_private::mcp::NearestNames(StringRef Wanted, ArrayRef<std::string> Names,
                                size_t Limit) {
  const StringRef WantedBase = BaseName(Wanted);
  const unsigned Tolerance = NameTolerance(WantedBase);

  SmallVector<std::pair<unsigned, StringRef>, 8> Ranked;
  for (StringRef Name : Names) {
    unsigned Distance = WantedBase.edit_distance(
        BaseName(Name), /*AllowReplacements=*/true, Tolerance);
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

namespace {

/// Parses one entry of `compare`.
Expected<RunVariant> ParseRunVariant(const json::Value &Value, size_t Index) {
  const json::Object *Obj = Value.getAsObject();
  const std::string Where = formatv("plan: compare[{0}]", Index).str();
  if (!Obj)
    return createStringError(
        formatv("{0} must be an object carrying \"label\" and the plan fields "
                "that differ in this run.",
                Where));
  if (Error E = UnrecognizedFields(*Obj, VariantFields, Where))
    return std::move(E);

  RunVariant Result;
  Expected<std::optional<std::string>> Label =
      GetNonEmptyString(*Obj, "label", Where);
  if (!Label)
    return Label.takeError();
  if (!*Label)
    return createStringError(
        formatv("{0}: \"label\" is required, because every result of a "
                "comparison is keyed on it.",
                Where));
  Result.Label = std::move(**Label);

  Expected<std::optional<std::string>> Program =
      GetNonEmptyString(*Obj, "program", Where);
  if (!Program)
    return Program.takeError();
  Result.Program = std::move(*Program);

  if (Obj->get("args")) {
    Expected<std::vector<std::string>> Args =
        GetStringArray(*Obj, "args", Where);
    if (!Args)
      return Args.takeError();
    Result.Args = std::move(*Args);
  }

  if (Obj->get("env")) {
    Expected<StringMap<std::string>> Env = GetEnv(*Obj, Where);
    if (!Env)
      return Env.takeError();
    Result.Env = std::move(*Env);
  }

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
  return Result;
}

} // namespace

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
  if (!*Program)
    return createStringError(
        "plan: \"program\" is required and is the path to the program to run.");
  // Separately from absent, because an empty string is *present* and the message
  // for the missing field denied it: a caller told that a field it had just
  // written is required goes looking for a second field of that name. Not routed
  // through `GetNonEmptyString`, whose message offers the default that leaving the
  // field out would get, because leaving this one out is the error above.
  if ((*Program)->empty())
    return createStringError(
        "plan: \"program\" is an empty string, which names no file. It is the "
        "path to the program to run.");
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
          formatv("plan: \"observe\" must be an array of observations, not {0}. "
                  "Leave it out to run the program with no tracepoints and "
                  "report only how it ended.",
                  AsWritten(*Observe))
              .str());
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

  if (const json::Value *Compare = Obj->get("compare")) {
    const json::Array *Array = Compare->getAsArray();
    if (!Array)
      return createStringError(
          formatv("plan: \"compare\" must be an array of runs, not {0}. Each is "
                  "a label and the fields of the plan that differ in it: "
                  "[{{\"label\": \"fixed\"}, {{\"label\": \"before\", "
                  "\"program\": \"/tmp/opt.before\"}]. Leave it out to make the "
                  "single run the plan describes.",
                  AsWritten(*Compare))
              .str());
    if (Array->size() == 1)
      return createStringError(
          "plan: \"compare\" holds one run, so there is nothing to compare it "
          "with. Leave it out for a single run, or name the run to compare "
          "against.");
    if (Array->size() > MaxComparedRuns)
      return createStringError(
          formatv("plan: \"compare\" holds {0} runs and at most {1} are made in "
                  "one call. Each is a launch and a debug-info read under the "
                  "plan's timeout, so a longer series is a series of calls whose "
                  "results are read one at a time.",
                  Array->size(), MaxComparedRuns));

    StringSet<> Labels;
    for (size_t I = 0; I < Array->size(); ++I) {
      Expected<RunVariant> Variant = ParseRunVariant((*Array)[I], I);
      if (!Variant)
        return Variant.takeError();
      if (!Labels.insert(Variant->Label).second)
        return createStringError(
            formatv("plan: two runs in \"compare\" are labelled \"{0}\". Every "
                    "result is keyed on the label, so they have to differ.",
                    Variant->Label));
      Result.Compare.push_back(std::move(*Variant));
    }
  }

  return Result;
}

ObservationPlan
lldb_private::mcp::ObservationPlan::WithVariant(const RunVariant &Variant) const {
  ObservationPlan Out = *this;
  if (Variant.Program)
    Out.Program = *Variant.Program;
  if (Variant.Args)
    Out.Args = *Variant.Args;
  if (Variant.Env)
    Out.Env = *Variant.Env;
  if (Variant.Cwd)
    Out.Cwd = Variant.Cwd;
  if (Variant.Stdin)
    Out.Stdin = Variant.Stdin;
  // Each run is made on its own, so the variants do not carry over into it.
  Out.Compare.clear();
  return Out;
}

namespace {

/// Whether the source a `file:line` resolved through has been written since the
/// binary holding its line table was, and by how much.
///
/// The file is taken from the resolved location rather than from what the caller
/// wrote, because that spelling may be a bare filename while the line table
/// holds the path the compiler saw -- and the compiler's copy is the one the line
/// number came from. The binary is likewise the module the location landed in,
/// which for a tracepoint in a shared library is that library and not the
/// program.
std::optional<std::string> DescribeSourceNewerThanBinary(Breakpoint &BP) {
  lldb::BreakpointLocationSP Loc = BP.GetLocationAtIndex(0);
  if (!Loc)
    return std::nullopt;

  SymbolContext SC;
  Loc->GetAddress().CalculateSymbolContext(
      &SC, lldb::eSymbolContextLineEntry | lldb::eSymbolContextModule);
  if (!SC.line_entry.IsValid() || !SC.module_sp)
    return std::nullopt;

  const FileSpec &Source = SC.line_entry.GetFile();
  FileSystem &FS = FileSystem::Instance();
  return DescribeSourceSkew(Source.GetFilename(),
                            FS.GetModificationTime(Source),
                            FS.GetModificationTime(SC.module_sp->GetFileSpec()));
}

} // namespace

std::optional<std::string> lldb_private::mcp::DescribeSourceSkew(
    StringRef Filename, sys::TimePoint<> Source, sys::TimePoint<> Binary) {
  // An unreadable mtime comes back as the epoch. Either file may be missing:
  // debug info records the path the compiler saw, which on a binary built
  // elsewhere names a directory this machine does not have. Nothing is reported
  // then, because a comparison that cannot be made is not a finding.
  if (!SourceSkewExceedsNoise(Source, Binary))
    return std::nullopt;

  // Stated as the fact it is -- these two mtimes, in this order -- and not as a
  // verdict that the line moved. Detecting *what* moved would need the source as
  // it was compiled, which nothing here has; and a source file may be newer than
  // a binary for reasons that changed no code, a checked-out tree being the
  // common one. The skew is what lets a reader tell the two apart: minutes is an
  // edit, days is a checkout.
  const auto Skew =
      std::chrono::duration_cast<std::chrono::seconds>(Source - Binary);
  std::string Ago;
  if (Skew < std::chrono::minutes(1))
    Ago = formatv("{0}s", Skew.count()).str();
  else if (Skew < std::chrono::hours(1))
    Ago = formatv("{0}m", Skew.count() / 60).str();
  else if (Skew < std::chrono::hours(48))
    Ago = formatv("{0}h", Skew.count() / 3600).str();
  else
    Ago = formatv("{0}d", Skew.count() / 86400).str();

  return formatv("\"{0}\" was written {1} after the binary holding its line "
                 "table, so this line is resolved from the older copy",
                 Filename, Ago)
      .str();
}

std::vector<LocationResolution>
lldb_private::mcp::ResolveObservationLocations(ObservationPlan &Plan,
                                               Target &Tgt) {
  std::vector<LocationResolution> Resolutions;
  Resolutions.reserve(Plan.Observations.size());


  for (const Observation &Obs : Plan.Observations) {
    LocationResolution Resolution;

    // Both trigger points resolve to the function's entry. A return
    // observation is derived from the entry hit, which keeps it independent of
    // where a compiler chose to put the epilogue.
    if (Obs.AtLine) {
      Resolution.Breakpoint = Tgt.CreateBreakpoint(
          /*containingModules=*/nullptr, FileSpec(Obs.At), *Obs.AtLine,
          /*column=*/0, /*offset=*/0, /*check_inlines=*/eLazyBoolCalculate,
          eLazyBoolCalculate, /*internal=*/false, /*request_hardware=*/false,
          /*move_to_nearest_code=*/eLazyBoolCalculate);
    } else {
      Resolution.Breakpoint = Tgt.CreateBreakpoint(
          /*containingModules=*/nullptr, /*containingSourceFiles=*/nullptr,
          Obs.At.c_str(), lldb::eFunctionNameTypeAuto,
          lldb::eLanguageTypeUnknown,
          /*offset=*/0, /*offset_is_insn_count=*/false, eLazyBoolCalculate,
          /*internal=*/false, /*request_hardware=*/false);
    }
    if (Resolution.Breakpoint)
      Resolution.ResolvedLocations =
          static_cast<uint32_t>(Resolution.Breakpoint->GetNumLocations());

    // Only for a line anchor. A function name does not depend on line numbers,
    // so editing its file moves nothing a tracepoint on it was pointing at.
    if (Obs.AtLine && Resolution.ResolvedLocations != 0)
      Resolution.SourceNewerThanBinary =
          DescribeSourceNewerThanBinary(*Resolution.Breakpoint);

    if (Resolution.ResolvedLocations == 0) {
      if (Obs.AtLine) {
        // Suggesting a similarly-spelled function explains nothing about a
        // source location, so say what actually goes wrong with one.
        Resolution.Error =
            formatv("\"{0}:{1}\" matched no code. Either no line table covers "
                    "that line, or the file is not part of this program. Check "
                    "the path as the compiler saw it, or name the enclosing "
                    "function instead, which does not depend on line numbers.",
                    Obs.At, *Obs.AtLine)
                .str();
      } else {
        // Drawn per name rather than once for the target: the shortlist is of
        // names that could be misspellings of this one, so it cannot be shared
        // with another.
        Resolution.Error =
            DescribeUnresolved(Obs.At, CollectCandidateNames(Tgt, Obs.At));
      }
    }

    Resolutions.push_back(std::move(Resolution));
  }

  return Resolutions;
}
