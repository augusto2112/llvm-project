//===-- ToolTest.cpp ------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Plugins/Protocol/MCP/Tool.h"
#include "lldb/Protocol/MCP/ObserveSurface.h"
#include "lldb/Protocol/MCP/Protocol.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <optional>
#include <string>

using namespace llvm;
using namespace lldb_private::mcp;
using namespace lldb_protocol::mcp;

namespace {

ObserveTool MakeTool() {
  return ObserveTool("trace_program", std::string(ObserveToolDescription));
}

/// The message a call failed with, so that a long piece of guidance can be
/// matched on the part of it that carries the fix.
std::string FailureMessage(Expected<CallToolResult> Result) {
  if (Result)
    return "unexpectedly succeeded";
  return toString(Result.takeError());
}

std::string Observe(json::Value Arguments) {
  ObserveTool Tool = MakeTool();
  ToolArguments Args = std::move(Arguments);
  return FailureMessage(Tool.Call(Args));
}

} // namespace

TEST(ObserveToolTest, RequiresArguments) {
  ObserveTool Tool = MakeTool();
  ToolArguments Args; // std::monostate
  // Matched whole rather than by substring, because what is being pinned is that
  // the message names the tool as the server advertises it. A class name is a
  // fact about this implementation, and a caller can neither look one up nor act
  // on it.
  EXPECT_EQ(FailureMessage(Tool.Call(Args)),
            "trace_program: no arguments. Pass \"plan\", the observation plan "
            "to run.");
}

TEST(ObserveToolTest, RequiresAnObject) {
  EXPECT_THAT(Observe(42), testing::HasSubstr("must be an object"));
}

TEST(ObserveToolTest, RequiresAPlan) {
  EXPECT_THAT(Observe(json::Object{}),
              testing::HasSubstr("\"plan\" is required"));
}

TEST(ObserveToolTest, PointsAFlattenedPlanAtTheNesting) {
  // The plan's own fields written as the arguments is the mistake the shape
  // invites, and the one that makes every call fail, so the error has to carry
  // the fix rather than a list of what was rejected.
  EXPECT_THAT(Observe(json::Object{{"program", "/bin/true"}}),
              testing::HasSubstr("the plan goes inside \"plan\""));
  EXPECT_THAT(Observe(json::Object{{"observe", json::Array{}}}),
              testing::HasSubstr("the plan goes inside \"plan\""));
}

TEST(ObserveToolTest, RejectsUnrecognizedArguments) {
  std::string Message = Observe(json::Object{
      {"plan", json::Object{{"program", "/bin/true"}}}, {"verbose", true}});
  EXPECT_THAT(Message, testing::HasSubstr("unrecognized argument \"verbose\""));
  EXPECT_THAT(Message, testing::HasSubstr("belongs inside \"plan\""));
}

TEST(ObserveToolTest, ReportsPlanErrors) {
  // The plan parser owns what a plan may say; the tool passes its message
  // through rather than restating it.
  EXPECT_THAT(Observe(json::Object{{"plan", json::Object{}}}),
              testing::HasSubstr("\"program\" is required"));
  EXPECT_THAT(
      Observe(json::Object{
          {"plan", json::Object{{"program", "/bin/true"},
                                {"observe", json::Array{json::Object{
                                                {"at", "0x100000f00"}}}}}}}),
      testing::HasSubstr("cannot be observed"));
}

TEST(ObserveToolTest, ReportsAMalformedDebugger) {
  EXPECT_EQ(
      Observe(json::Object{{"plan", json::Object{{"program", "/bin/true"}}},
                           {"debugger", "notanumber"}}),
      "malformed debugger specifier notanumber");
}

TEST(ObserveToolTest, SchemaNestsThePlanUnderItsOwnKey) {
  ObserveTool Tool = MakeTool();
  std::optional<json::Value> Schema = Tool.GetSchema();
  ASSERT_TRUE(Schema.has_value());
  const json::Object *Root = Schema->getAsObject();
  ASSERT_NE(Root, nullptr);

  // The plan is nested because ParseObservationPlan rejects any field it does
  // not define, and `debugger` is not one of them. A schema that flattened the
  // two would make every call fail.
  ASSERT_NE(Root->getArray("required"), nullptr);
  EXPECT_EQ(*Root->getArray("required"), json::Array{"plan"});
  EXPECT_EQ(Root->getBoolean("additionalProperties"), false);

  const json::Object *Properties = Root->getObject("properties");
  ASSERT_NE(Properties, nullptr);
  EXPECT_NE(Properties->getObject("debugger"), nullptr);

  const json::Object *Plan = Properties->getObject("plan");
  ASSERT_NE(Plan, nullptr);
  ASSERT_NE(Plan->getArray("required"), nullptr);
  EXPECT_EQ(*Plan->getArray("required"), json::Array{"program"});
  EXPECT_EQ(Plan->getBoolean("additionalProperties"), false);

  const json::Object *PlanProperties = Plan->getObject("properties");
  ASSERT_NE(PlanProperties, nullptr);
  const json::Object *Observe = PlanProperties->getObject("observe");
  ASSERT_NE(Observe, nullptr);
  const json::Object *Observation = Observe->getObject("items");
  ASSERT_NE(Observation, nullptr);
  ASSERT_NE(Observation->getArray("required"), nullptr);
  EXPECT_EQ(*Observation->getArray("required"), json::Array{"at"});

  // Every field the plan parser accepts is described, so that a caller reading
  // the schema never has to guess at one.
  const json::Object *ObservationProperties =
      Observation->getObject("properties");
  ASSERT_NE(ObservationProperties, nullptr);
  for (StringRef Field :
       {"at", "label", "on", "capture", "when", "called_from", "enabled_after",
        "skip_first", "only_hit", "emit", "backtrace", "depth"})
    EXPECT_NE(ObservationProperties->get(Field), nullptr) << Field;
  for (StringRef Field :
       {"program", "args", "env", "cwd", "stdin", "capture_inferior_output",
        "fast", "timeout_seconds", "no_progress_seconds", "observe"})
    EXPECT_NE(PlanProperties->get(Field), nullptr) << Field;
}
