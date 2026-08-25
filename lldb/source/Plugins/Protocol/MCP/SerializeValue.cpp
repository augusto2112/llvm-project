//===- SerializeValue.cpp -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SerializeValue.h"
#include "ValueNode.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace lldb_private::mcp;
using namespace llvm;

namespace {

StringRef AvailabilityName(Availability A) {
  switch (A) {
  case Availability::Available:
    return "";
  case Availability::OptimizedOut:
    return "optimized_out";
  case Availability::NoLocation:
    return "no_location";
  case Availability::NoDebugInfo:
    return "no_debug_info";
  case Availability::Error:
    return "error";
  }
  llvm_unreachable("unhandled Availability");
}

/// Shortens \p S to \p Max bytes and marks the cut. json::Value requires valid
/// UTF-8, which neither a byte-wise cut of a multi-byte sequence nor a raw
/// value read out of an inferior is guaranteed to be, so the result is
/// repaired before it can reach a json::Value constructor.
std::string Truncate(std::string S, unsigned Max) {
  if (S.size() > Max) {
    S.resize(Max);
    S += "...";
  }
  if (!json::isUTF8(S))
    S = json::fixUTF8(S);
  return S;
}

/// Whether \p Line is one of the two lines clang prints under a diagnostic to
/// point at the offending column: the echoed source, `    1 | nosuch(n)`, and
/// the carets under it. Both are laid out for a reader looking at a terminal and
/// carry nothing a reader of one line needs.
bool IsCaretArt(StringRef Line) {
  if (Line.contains(" | "))
    return true;
  return Line.find_first_not_of("^~| \t") == StringRef::npos;
}

/// The reason every one of \p Children could not be read, or empty when any of
/// them could be, or when they failed for different reasons. A reason shared by
/// all of them belongs to the node above rather than to each of them.
StringRef
SoleUnavailableReason(ArrayRef<std::pair<std::string, json::Value>> Children) {
  StringRef Shared;
  for (const auto &[Name, Value] : Children) {
    const json::Object *Obj = Value.getAsObject();
    if (!Obj || !Obj->get("unavailable"))
      return StringRef();
    std::optional<StringRef> Reason = Obj->getString("reason");
    if (!Reason || Reason->empty())
      return StringRef();
    if (Shared.empty())
      Shared = *Reason;
    else if (Shared != *Reason)
      return StringRef();
  }
  return Shared;
}

struct Serializer {
  const SerializeValueOptions &Opts;
  unsigned Budget;

  /// Identities already emitted. A second appearance of the same object is
  /// reported instead of expanded, which is what keeps the walk finite over a
  /// value graph.
  SmallSet<uint64_t, 32> Seen;

  explicit Serializer(const SerializeValueOptions &O)
      : Opts(O), Budget(O.SharedBudget ? *O.SharedBudget : O.MaxNodes) {}

  json::Value Elided(std::string Why) {
    json::Object O{{"_elided", std::move(Why)}};
    if (!Opts.ArtifactRef.empty())
      O["_more"] = Opts.ArtifactRef;
    return O;
  }

  json::Value Run(ValueNode &N, unsigned Depth) {
    return Run(N, Depth, StringRef());
  }

  json::Value Run(ValueNode &N, unsigned Depth, StringRef ParentReason) {
    if (Budget == 0)
      return Elided("node budget");
    --Budget;

    Availability A = N.GetAvailability();
    std::string Why;
    if (A != Availability::Available) {
      json::Object Out{{"unavailable", AvailabilityName(A)}};
      // The kind says which group the failure is in and the reason says which
      // failure it is, which is what decides what the reader does next. It costs
      // nothing per hit: identical renderings collapse in the aggregate, and a
      // capture that keeps failing is switched off after a few hits.
      //
      // Said once per cause rather than once per node: every member of an object
      // read through a null pointer is unreadable for the one reason the pointer
      // gives, and repeating it produced twelve copies of "parent is NULL" under
      // one capture.
      Why = N.GetUnavailableReason();
      if (!Why.empty() && Why != ParentReason)
        Out["reason"] = Truncate(Why, Opts.MaxStringLength);
      return Out;
    }

    // A formatter summary is the dense rendering, so it stands in for the
    // subtree: expanding children past a good summary costs tokens and adds
    // nothing.
    if (std::optional<std::string> Summary = N.GetSummary()) {
      if (Opts.SawSummary)
        *Opts.SawSummary = true;
      return json::Object{
          {"summary", Truncate(std::move(*Summary), Opts.MaxStringLength)}};
    }

    uint64_t Id = N.GetIdentity();
    if (Id != 0 && !Seen.insert(Id).second)
      return json::Object{{"_cycle", "already reported above"}};

    size_t NumChildren = N.GetNumChildren();
    if (NumChildren == 0 || Depth >= Opts.MaxDepth) {
      if (std::optional<std::string> V = N.GetValueString())
        return json::Object{
            {"value", Truncate(std::move(*V), Opts.MaxStringLength)}};
      if (NumChildren != 0)
        return Elided(formatv("{0} children", NumChildren).str());
      return json::Object{{"value", ""}};
    }

    // Reached only past the summary check above, so this is a node that has
    // members and nothing that renders it as one thing. Recorded so a run can say
    // once that it met such a value and no formatter matched anything, which is
    // the only way a caller can tell "this type has no custom rendering" from
    // "the formatters were never loaded".
    if (Opts.SawExpansion)
      *Opts.SawExpansion = true;

    json::Object Out;

    // A node with children may still hold a value of its own: a pointer's value
    // is the address, and its child is whatever that address points at. Dropping
    // it in favour of the child loses the more important half — for a null
    // pointer the address is the whole answer, and the child is unreadable
    // precisely because of it.
    if (std::optional<std::string> V = N.GetValueString())
      Out["value"] = Truncate(std::move(*V), Opts.MaxStringLength);

    size_t Emit = std::min<size_t>(NumChildren, Opts.MaxChildren);
    SmallVector<std::pair<std::string, json::Value>, 8> Children;
    for (size_t I = 0; I < Emit; ++I) {
      std::unique_ptr<ValueNode> Child = N.GetChildAtIndex(I);
      if (!Child)
        continue;
      std::string Name = Child->GetName().str();
      if (Name.empty())
        Name = formatv("[{0}]", I).str();
      Children.emplace_back(std::move(Name), Run(*Child, Depth + 1, Why));
    }

    // Children that are all unreadable for one reason are one fact rather than
    // one fact per child. A pointer that is null has an unreadable member for
    // every member the type has: measured on a capture of one such pointer into a
    // compiler's value hierarchy, twelve copies of "parent is NULL", none of
    // which said anything the pointer's own value had not already said.
    if (!Children.empty()) {
      StringRef Shared = SoleUnavailableReason(Children);
      if (!Shared.empty()) {
        Out["_elided"] =
            formatv("{0} children, none readable: {1}", NumChildren, Shared)
                .str();
        return Out;
      }
    }

    for (auto &[Name, Value] : Children)
      Out[Name] = std::move(Value);
    if (Emit < NumChildren)
      Out["_elided"] = formatv("{0} more children", NumChildren - Emit).str();
    return Out;
  }
};

} // namespace

std::string lldb_private::mcp::CondenseDiagnostic(StringRef Message,
                                                  unsigned MaxLength) {
  SmallVector<StringRef, 8> Lines;
  Message.split(Lines, '\n');

  StringRef Head, Error, Hint;
  for (StringRef Line : Lines) {
    Line = Line.trim();
    if (Line.empty() || IsCaretArt(Line))
      continue;

    // Kept whichever line it appears on, and looked for over the whole message
    // rather than until the diagnostic is found: for a call into a symbol the
    // target does not hold, the hint follows the diagnostic and is the half that
    // names what to do about it.
    if (Line.starts_with("Hint:")) {
      if (Hint.empty())
        Hint = Line.drop_front(5).ltrim();
      continue;
    }

    // An `error:` line is preferred wherever it appears, because the lines
    // before it are the evaluator describing itself: an expression diagnostic
    // opens with "Ran expression as 'C++14'.", which is true of every expression
    // and says nothing about the one that failed. A message with no such line --
    // "Couldn't look up symbols:" -- is itself the diagnostic, so the first line
    // stands in.
    if (Line.consume_front("error:")) {
      if (Error.empty())
        Error = Line.ltrim();
      continue;
    }
    if (Head.empty())
      Head = Line;
  }
  if (!Error.empty())
    Head = Error;

  // A `<user expression 1>:1:17:` prefix locates the error inside an expression
  // the caller never sees a listing of, so the position is unusable and the text
  // after it is the whole content.
  if (Head.starts_with("<")) {
    if (size_t Close = Head.find('>'); Close != StringRef::npos) {
      StringRef After = Head.drop_front(Close + 1);
      // Consumed only as far as a line:column actually reaches, and applied only
      // if one was there: a diagnostic that merely opens with a bracketed word,
      // "<invalid> is not a type", is all content and keeps its first word.
      bool Positioned = false;
      while (After.consume_front(":")) {
        size_t Digits = After.find_first_not_of("0123456789");
        if (Digits == 0)
          break;
        Positioned = true;
        After = Digits == StringRef::npos ? StringRef() : After.substr(Digits);
      }
      if (Positioned)
        Head = After.ltrim();
    }
  }

  std::string Out = Head.str();
  if (!Hint.empty()) {
    if (!Out.empty())
      Out += ' ';
    Out += Hint.str();
  }
  return Truncate(std::move(Out), MaxLength);
}

json::Value
lldb_private::mcp::SerializeValue(ValueNode &Root,
                                  const SerializeValueOptions &Opts) {
  Serializer S(Opts);
  json::Value Out = S.Run(Root, 0);
  if (Opts.SharedBudget)
    *Opts.SharedBudget = S.Budget;

  if (Opts.MaxRenderedChars == 0)
    return Out;

  std::string Rendered;
  raw_string_ostream OS(Rendered);
  OS << Out;
  if (Rendered.size() <= Opts.MaxRenderedChars)
    return Out;

  // Re-read at depth zero rather than truncated as text, so that what comes back
  // is a value and not a fragment of one. The second pass walks no children, so
  // it costs no further reads into the observed process.
  SerializeValueOptions Shallow = Opts;
  Shallow.MaxDepth = 0;
  Shallow.MaxRenderedChars = 0;
  Shallow.SharedBudget = nullptr;
  Serializer Reduced(Shallow);
  json::Value Small = Reduced.Run(Root, 0);
  if (json::Object *Obj = Small.getAsObject())
    Obj->try_emplace("_elided", formatv("{0} characters of members; capture a "
                                        "path naming the one wanted",
                                        Rendered.size())
                                    .str());
  return Small;
}
