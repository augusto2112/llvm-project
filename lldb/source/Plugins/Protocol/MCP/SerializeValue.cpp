//===- SerializeValue.cpp -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SerializeValue.h"
#include "ValueNode.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
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

struct Serializer {
  const SerializeValueOptions &Opts;
  unsigned Budget;

  /// Identities already emitted. A second appearance of the same object is
  /// reported instead of expanded, which is what keeps the walk finite over a
  /// value graph.
  SmallSet<uint64_t, 32> Seen;

  explicit Serializer(const SerializeValueOptions &O)
      : Opts(O), Budget(O.MaxNodes) {}

  json::Value Elided(std::string Why) {
    json::Object O{{"_elided", std::move(Why)}};
    if (!Opts.ArtifactRef.empty())
      O["_more"] = Opts.ArtifactRef;
    return O;
  }

  json::Value Run(ValueNode &N, unsigned Depth) {
    if (Budget == 0)
      return Elided("node budget");
    --Budget;

    Availability A = N.GetAvailability();
    if (A != Availability::Available)
      return json::Object{{"unavailable", AvailabilityName(A)}};

    // A formatter summary is the dense rendering, so it stands in for the
    // subtree: expanding children past a good summary costs tokens and adds
    // nothing.
    if (std::optional<std::string> Summary = N.GetSummary())
      return json::Object{
          {"summary", Truncate(std::move(*Summary), Opts.MaxStringLength)}};

    uint64_t Id = N.GetIdentity();
    if (Id != 0 && !Seen.insert(Id).second)
      return json::Object{{"_cycle", formatv("0x{0:x}", Id).str()}};

    size_t NumChildren = N.GetNumChildren();
    if (NumChildren == 0 || Depth >= Opts.MaxDepth) {
      if (std::optional<std::string> V = N.GetValueString())
        return json::Object{
            {"value", Truncate(std::move(*V), Opts.MaxStringLength)}};
      if (NumChildren != 0)
        return Elided(formatv("{0} children", NumChildren).str());
      return json::Object{{"value", ""}};
    }

    json::Object Out;

    // A node with children may still hold a value of its own: a pointer's value
    // is the address, and its child is whatever that address points at. Dropping
    // it in favour of the child loses the more important half — for a null
    // pointer the address is the whole answer, and the child is unreadable
    // precisely because of it.
    if (std::optional<std::string> V = N.GetValueString())
      Out["value"] = Truncate(std::move(*V), Opts.MaxStringLength);

    size_t Emit = std::min<size_t>(NumChildren, Opts.MaxChildren);
    for (size_t I = 0; I < Emit; ++I) {
      std::unique_ptr<ValueNode> Child = N.GetChildAtIndex(I);
      if (!Child)
        continue;
      std::string Name = Child->GetName().str();
      if (Name.empty())
        Name = formatv("[{0}]", I).str();
      json::Value ChildValue = Run(*Child, Depth + 1);
      Out[std::move(Name)] = std::move(ChildValue);
    }
    if (Emit < NumChildren)
      Out["_elided"] = formatv("{0} more children", NumChildren - Emit).str();
    return Out;
  }
};

} // namespace

json::Value
lldb_private::mcp::SerializeValue(ValueNode &Root,
                                  const SerializeValueOptions &Opts) {
  Serializer S(Opts);
  return S.Run(Root, 0);
}
