//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Plugins/Protocol/MCP/SerializeValue.h"
#include "Plugins/Protocol/MCP/ValueNode.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace lldb_private::mcp;

namespace {

/// A hand-built value tree, so serialization can be tested without a process.
class FakeNode : public ValueNode {
public:
  std::string Name, TypeName, Summary, Value, Reason;
  Availability Avail = Availability::Available;
  uint64_t Identity = 0;
  std::vector<std::shared_ptr<FakeNode>> Children;

  llvm::StringRef GetName() override { return Name; }
  llvm::StringRef GetTypeName() override { return TypeName; }
  std::optional<std::string> GetSummary() override {
    if (Summary.empty())
      return std::nullopt;
    return Summary;
  }
  std::optional<std::string> GetValueString() override {
    if (Value.empty())
      return std::nullopt;
    return Value;
  }
  Availability GetAvailability() override { return Avail; }
  std::string GetUnavailableReason() override { return Reason; }
  uint64_t GetIdentity() override { return Identity; }
  size_t GetNumChildren() override { return Children.size(); }
  std::unique_ptr<ValueNode> GetChildAtIndex(size_t Idx) override;
};

/// Non-owning view onto a FakeNode, so a tree can be walked repeatedly.
class FakeNodeRef : public ValueNode {
public:
  explicit FakeNodeRef(std::shared_ptr<FakeNode> N) : N(std::move(N)) {}
  llvm::StringRef GetName() override { return N->Name; }
  llvm::StringRef GetTypeName() override { return N->TypeName; }
  std::optional<std::string> GetSummary() override { return N->GetSummary(); }
  std::optional<std::string> GetValueString() override {
    return N->GetValueString();
  }
  Availability GetAvailability() override { return N->Avail; }
  std::string GetUnavailableReason() override { return N->Reason; }
  uint64_t GetIdentity() override { return N->Identity; }
  size_t GetNumChildren() override { return N->Children.size(); }
  std::unique_ptr<ValueNode> GetChildAtIndex(size_t Idx) override {
    return std::make_unique<FakeNodeRef>(N->Children[Idx]);
  }

private:
  std::shared_ptr<FakeNode> N;
};

std::unique_ptr<ValueNode> FakeNode::GetChildAtIndex(size_t Idx) {
  return std::make_unique<FakeNodeRef>(Children[Idx]);
}

std::shared_ptr<FakeNode> MakeLeaf(std::string Name, std::string Value,
                                   uint64_t Id = 0) {
  auto N = std::make_shared<FakeNode>();
  N->Name = std::move(Name);
  N->Value = std::move(Value);
  N->Identity = Id;
  return N;
}

std::string ToString(const llvm::json::Value &V) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  OS << V;
  return S;
}

} // namespace

TEST(SerializeValueTest, LeafEmitsValueString) {
  auto Root = MakeLeaf("i", "42");
  FakeNodeRef Ref(Root);
  EXPECT_EQ(ToString(SerializeValue(Ref, {})), R"({"value":"42"})");
}

TEST(SerializeValueTest, SummaryPreemptsChildren) {
  auto Root = std::make_shared<FakeNode>();
  Root->Name = "V";
  Root->Summary = "add i32 %a, %b";
  Root->Children.push_back(MakeLeaf("Op0", "1"));
  Root->Children.push_back(MakeLeaf("Op1", "2"));

  FakeNodeRef Ref(Root);
  // A summary is the dense rendering; children must not be expanded.
  EXPECT_EQ(ToString(SerializeValue(Ref, {})),
            R"({"summary":"add i32 %a, %b"})");
}

TEST(SerializeValueTest, ExpandsChildrenToMaxDepth) {
  auto Leaf = MakeLeaf("c", "3");
  auto Mid = std::make_shared<FakeNode>();
  Mid->Name = "b";
  Mid->Children.push_back(Leaf);
  auto Root = std::make_shared<FakeNode>();
  Root->Name = "a";
  Root->Children.push_back(Mid);

  SerializeValueOptions Opts;
  Opts.MaxDepth = 1;
  FakeNodeRef Ref(Root);
  // Depth 1 reaches "b" but must not reach "c".
  std::string S = ToString(SerializeValue(Ref, Opts));
  EXPECT_NE(S.find("\"b\""), std::string::npos);
  EXPECT_EQ(S.find("\"c\""), std::string::npos);
  EXPECT_NE(S.find("_elided"), std::string::npos);
}

TEST(SerializeValueTest, ElisionMarkerPointsAtArtifact) {
  auto Leaf = MakeLeaf("c", "3");
  auto Root = std::make_shared<FakeNode>();
  Root->Name = "a";
  Root->Children.push_back(Leaf);

  SerializeValueOptions Opts;
  Opts.MaxDepth = 0;
  Opts.ArtifactRef = "$artifact#seq=7";
  FakeNodeRef Ref(Root);
  // An elision that is a dead end forces a re-run; it must be a redirect.
  EXPECT_NE(ToString(SerializeValue(Ref, Opts)).find("$artifact#seq=7"),
            std::string::npos);
}

TEST(SerializeValueTest, StopsAtNodeBudget) {
  auto Root = std::make_shared<FakeNode>();
  Root->Name = "a";
  for (int I = 0; I < 50; ++I)
    Root->Children.push_back(MakeLeaf("c" + std::to_string(I), "0"));

  SerializeValueOptions Opts;
  Opts.MaxDepth = 4;
  Opts.MaxNodes = 5;
  FakeNodeRef Ref(Root);
  std::string S = ToString(SerializeValue(Ref, Opts));
  // Depth is not a bound on a graph; the node budget is.
  EXPECT_NE(S.find("_elided"), std::string::npos);
  EXPECT_EQ(S.find("\"c40\""), std::string::npos);
}

TEST(SerializeValueTest, TruncatesLongStrings) {
  auto Root = MakeLeaf("s", std::string(500, 'x'));
  SerializeValueOptions Opts;
  Opts.MaxStringLength = 8;
  FakeNodeRef Ref(Root);
  std::string S = ToString(SerializeValue(Ref, Opts));
  EXPECT_NE(S.find("xxxxxxxx"), std::string::npos);
  EXPECT_EQ(S.find(std::string(20, 'x')), std::string::npos);
}

TEST(SerializeValueTest, DetectsCycles) {
  auto Root = std::make_shared<FakeNode>();
  Root->Name = "n";
  Root->Identity = 0x1000;
  auto Next = std::make_shared<FakeNode>();
  Next->Name = "next";
  Next->Identity = 0x1000; // same object: a cycle
  Root->Children.push_back(Next);

  SerializeValueOptions Opts;
  Opts.MaxDepth = 6;
  FakeNodeRef Ref(Root);
  EXPECT_NE(ToString(SerializeValue(Ref, Opts)).find("_cycle"),
            std::string::npos);
}

TEST(SerializeValueTest, DistinguishesUnavailabilityKinds) {
  // "no value here" and "no value anywhere" must not look the same, or the
  // agent cannot tell an optimized-out variable from a null one.
  auto A = MakeLeaf("a", "");
  A->Avail = Availability::OptimizedOut;
  FakeNodeRef RefA(A);
  EXPECT_EQ(ToString(SerializeValue(RefA, {})),
            R"({"unavailable":"optimized_out"})");

  auto B = MakeLeaf("b", "");
  B->Avail = Availability::NoDebugInfo;
  FakeNodeRef RefB(B);
  EXPECT_EQ(ToString(SerializeValue(RefB, {})),
            R"({"unavailable":"no_debug_info"})");
}

TEST(SerializeValueTest, NodeWithChildrenKeepsItsOwnValue) {
  // A pointer's value is the address and its child is the pointee. For a null
  // pointer the address is the answer and the child is unreadable because of
  // it, so reporting only the child loses the more important half.
  auto Pointee = MakeLeaf("*p", "");
  Pointee->Avail = Availability::Error;
  auto Root = MakeLeaf("p", "0x0");
  Root->Children.push_back(Pointee);

  FakeNodeRef Ref(Root);
  std::string S = ToString(SerializeValue(Ref, {}));
  EXPECT_NE(S.find("\"value\":\"0x0\""), std::string::npos) << S;
  EXPECT_NE(S.find("\"*p\""), std::string::npos) << S;
}

TEST(SerializeValueTest, UnavailableValueCarriesItsReason) {
  // The kind groups the failure and the reason names it. Without the reason a
  // reader cannot tell a misspelled capture from one that is out of scope from
  // one whose symbol the binary does not hold, and all three are spelled
  // "error".
  auto A = MakeLeaf("a", "");
  A->Avail = Availability::Error;
  A->Reason = "use of undeclared identifier 'nosuch'";
  FakeNodeRef Ref(A);
  EXPECT_EQ(ToString(SerializeValue(Ref, {})),
            R"({"reason":"use of undeclared identifier 'nosuch'",)"
            R"("unavailable":"error"})");
}

TEST(SerializeValueTest, UnavailableValueWithNoReasonReportsOnlyTheKind) {
  // A value the debugger has nothing to say about must not grow an empty field
  // for the sake of a uniform shape.
  auto A = MakeLeaf("a", "");
  A->Avail = Availability::OptimizedOut;
  FakeNodeRef Ref(A);
  EXPECT_EQ(ToString(SerializeValue(Ref, {})),
            R"({"unavailable":"optimized_out"})");
}

TEST(SerializeValueTest, ReasonIsTruncatedLikeAnyOtherString) {
  auto A = MakeLeaf("a", "");
  A->Avail = Availability::Error;
  A->Reason = std::string(500, 'z');
  SerializeValueOptions Opts;
  Opts.MaxStringLength = 8;
  FakeNodeRef Ref(A);
  // A capture that fails at every hit of a hot tracepoint must not be able to
  // spend the response on one repeated diagnostic.
  EXPECT_EQ(ToString(SerializeValue(Ref, Opts)),
            R"({"reason":"zzzzzzzz...","unavailable":"error"})");
}

TEST(SerializeValueTest, SharedBudgetIsSpentAcrossSeveralValues) {
  // Every local of a frame goes into one response, so the response is what has
  // to be bounded. Given a budget of its own, one local that reaches a large
  // object spends as much as all the others put together.
  auto Wide = std::make_shared<FakeNode>();
  Wide->Name = "wide";
  for (int I = 0; I < 20; ++I)
    Wide->Children.push_back(MakeLeaf("c" + std::to_string(I), "0"));
  auto Scalar = MakeLeaf("n", "42");

  unsigned Budget = 6;
  SerializeValueOptions Opts;
  Opts.SharedBudget = &Budget;

  FakeNodeRef WideRef(Wide);
  const std::string First = ToString(SerializeValue(WideRef, Opts));
  EXPECT_NE(First.find("_elided"), std::string::npos) << First;
  EXPECT_EQ(Budget, 0u);

  // The second value arrives after the budget is gone, and says so rather than
  // being dropped without a trace.
  FakeNodeRef ScalarRef(Scalar);
  EXPECT_NE(ToString(SerializeValue(ScalarRef, Opts)).find("_elided"),
            std::string::npos);

  // Without sharing, the same scalar renders in full: the bound is the shared
  // budget and not a property of the value.
  EXPECT_EQ(ToString(SerializeValue(ScalarRef, {})), R"({"value":"42"})");
}

TEST(CondenseDiagnosticTest, ExpressionDiagnosticKeepsOnlyTheErrorLine) {
  // The evaluator opens with what language it chose, which is true of every
  // expression, and closes with clang's caret art, which is laid out for a
  // terminal. Neither survives.
  const std::string Message = "Ran expression as 'C++14'.\n"
                              "error: <user expression 1>:1:1: use of "
                              "undeclared identifier 'nosuch'\n"
                              "    1 | nosuch(n)\n"
                              "      | ^~~~~~\n";
  EXPECT_EQ(CondenseDiagnostic(Message, 200),
            "use of undeclared identifier 'nosuch'");
}

TEST(CondenseDiagnosticTest, HintAfterTheDiagnosticIsKept) {
  // The hint follows the diagnostic, and for this failure it is the half that
  // says what to do: the function is not in the binary, so no spelling of the
  // call will work.
  const std::string Message =
      "Couldn't look up symbols:\n"
      "  $__lldb_func::0x1:0xbef:_ZNK4Node8describeEv\n"
      "Hint: The expression tried to call a function that is not present in "
      "the target.\n";
  EXPECT_EQ(CondenseDiagnostic(Message, 200),
            "Couldn't look up symbols: The expression tried to call a function "
            "that is not present in the target.");
}

TEST(CondenseDiagnosticTest, MessageWithNothingToStripIsKeptWhole) {
  EXPECT_EQ(CondenseDiagnostic("parent is NULL", 200), "parent is NULL");
  EXPECT_EQ(CondenseDiagnostic("", 200), "");
}

TEST(CondenseDiagnosticTest, BracketedWordIsNotMistakenForAPositionPrefix) {
  // Only a bracket followed by line:column is a position, so a diagnostic that
  // merely opens with a bracketed word keeps all of itself.
  EXPECT_EQ(CondenseDiagnostic("error: <invalid> is not a type", 200),
            "<invalid> is not a type");
}
