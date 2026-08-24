//===-- ObserveSurface.cpp ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Protocol/MCP/ObserveSurface.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include <cstdint>
#include <utility>

using namespace llvm;

namespace {

json::Value schemaField(StringRef type, StringRef description) {
  return json::Object{{"type", type}, {"description", description}};
}

json::Value schemaStringArray(StringRef description) {
  return json::Object{{"type", "array"},
                      {"items", json::Object{{"type", "string"}}},
                      {"description", description}};
}

json::Value schemaEnum(json::Array values, StringRef fallback,
                       StringRef description) {
  return json::Object{{"type", "string"},
                      {"enum", std::move(values)},
                      {"default", fallback},
                      {"description", description}};
}

json::Value schemaNumber(int64_t fallback, StringRef description) {
  return json::Object{
      {"type", "integer"}, {"default", fallback}, {"description", description}};
}

} // namespace

json::Value lldb_protocol::mcp::ObservationSchema() {
  return json::Object{
      {"type", "object"},
      {"properties",
       json::Object{
           {"at",
            schemaField("string",
                        "Where to observe, as a function name -- qualified or "
                        "not, \"llvm::SROA::runOnAlloca\" -- or a source "
                        "location written as \"file.cpp:1189\". An address is "
                        "rejected because it cannot be carried from one run "
                        "into the next.")},
           {"label",
            schemaField("string",
                        "Identifies this observation in the report and in "
                        "another observation's \"enabled_after\". Defaults to "
                        "\"at\", so it is only worth setting when one function "
                        "is observed more than once.")},
           {"on", schemaEnum(json::Array{"entry", "return"}, "entry",
                             "Whether state is read as the function returns "
                             "rather than as it is entered. At \"return\" the "
                             "frame is already gone: capture \"$return\", since "
                             "the function's own parameters and locals cannot "
                             "be read there.")},
           {"capture",
            schemaStringArray(
                "Expressions to read at each hit, in the language of the "
                "program: a member path like \"I.Ty.TypeID\" or \"N->Opcode\", "
                "or a call like \"describe(N)\". A call to the program's own "
                "printer, \"N->dump()\", is how an object that knows how to "
                "describe itself is read; what it prints comes back in "
                "\"inferior_output\" rather than as the capture's value. Empty is "
                "a bare tracepoint recording only hit counts, which already "
                "answers whether the code runs at all.")},
           {"when",
            schemaField("string",
                        "A condition evaluated at each hit, written like a "
                        "capture. A hit whose condition is false still counts "
                        "as a hit, but is neither captured nor emitted.")},
           {"called_from",
            schemaField("string", "Restricts the tracepoint to hits reached "
                                  "from this function.")},
           {"enabled_after",
            schemaField("string",
                        "Holds this observation disabled until the observation "
                        "carrying this label has been hit.")},
           {"skip_first",
            schemaNumber(0, "Hits to ignore before the tracepoint starts "
                            "recording.")},
           {"only_hit",
            schemaField("integer",
                        "Records a single hit, counting from one. This is the "
                        "follow-up to an aggregate that named an interesting "
                        "hit: that one hit comes back in full detail.")},
           {"emit",
            schemaEnum(json::Array{"every_hit", "on_change", "first_and_last"},
                       "every_hit",
                       "Which hits of this observation reach the event stream. "
                       "Reducing the stream never loses counts: aggregation "
                       "runs over every hit regardless of the mode.")},
           {"backtrace",
            schemaNumber(0, "Frames of backtrace to record per emitted "
                            "event.")},
           {"depth", schemaNumber(2, "Levels of children to expand in each "
                                     "captured value.")},
       }},
      {"required", json::Array{"at"}},
      {"additionalProperties", false},
  };
}

json::Value lldb_protocol::mcp::ObservationPlanSchema() {
  return json::Object{
      {"type", "object"},
      {"description", "What to run and what to observe while it runs."},
      {"properties",
       json::Object{
           {"program", schemaField("string", "The path to the program to "
                                             "run.")},
           {"args", schemaStringArray("Arguments passed to the program.")},
           {"env",
            json::Object{
                {"type", "object"},
                {"additionalProperties", json::Object{{"type", "string"}}},
                {"description",
                 "Environment variables set for the program, added to the "
                 "environment it would otherwise inherit. Values must be "
                 "strings; a number or boolean is not converted to one."}}},
           {"cwd", schemaField("string", "The directory to run the program "
                                         "in.")},
           {"stdin",
            schemaField("string", "A path whose contents are fed to the "
                                  "program's standard input.")},
           {"capture_inferior_output",
            json::Object{
                {"type", "boolean"},
                {"default", true},
                {"description",
                 "Whether the program's own standard output and error are "
                 "recorded. A program's last line of output is often the only "
                 "evidence of how far it got, so this is on unless it is "
                 "turned off."}}},
           {"timeout_seconds",
            schemaNumber(30,
                         "Wall-clock ceiling on the program, measured from the "
                         "launch: reading its debug info happens first and is "
                         "reported separately as \"setup_ms\", so a large binary "
                         "does not spend the budget for running it. Reaching the "
                         "ceiling is a result and not an error: the run comes "
                         "back with \"outcome\": \"timed_out\", everything "
                         "observed up to that point, and where the program was "
                         "stopped.")},
           {"no_progress_seconds",
            schemaField(
                "integer",
                "Gives up after this long with no tracepoint in the "
                "plan being hit at all, and must be shorter than "
                "\"timeout_seconds\". Absent leaves the check "
                "disarmed, because a plan whose triggers only fire near "
                "the end of a run is legitimate and would otherwise be "
                "cut short.")},
           {"observe",
            json::Object{
                {"type", "array"},
                {"items", ObservationSchema()},
                {"description",
                 "The tracepoints. An absent or empty list is a legal plan: it "
                 "runs the program and reports only how it ended, which is "
                 "crash triage."}}},
           {"compare",
            json::Object{
                {"type", "array"},
                {"items",
                 json::Object{
                     {"type", "object"},
                     {"properties",
                      json::Object{
                          {"label",
                           schemaField("string",
                                       "Names this run in the report. Every "
                                       "result is keyed on it.")},
                          {"program", schemaField("string", "Overrides the "
                                                            "plan's program.")},
                          {"args", schemaStringArray("Overrides the plan's "
                                                     "arguments.")},
                          {"env",
                           json::Object{
                               {"type", "object"},
                               {"additionalProperties",
                                json::Object{{"type", "string"}}},
                               {"description", "Overrides the plan's "
                                               "environment."}}},
                          {"cwd", schemaField("string", "Overrides the plan's "
                                                        "directory.")},
                          {"stdin", schemaField("string", "Overrides the plan's "
                                                          "standard input.")},
                      }},
                     {"required", json::Array{"label"}},
                     {"additionalProperties", false},
                 }},
                {"description",
                 "Runs to make and compare, each one this plan with these fields "
                 "replaced: the same tracepoints over the binary before and "
                 "after a change, or over the input that fails and the one that "
                 "does not. The response is the differences rather than one "
                 "report per run -- how each ended, the hits and values that "
                 "disagreed, the first hit at which they stopped agreeing, and "
                 "the names of everything that matched. Leave it out for a "
                 "single run."}}},
       }},
      {"required", json::Array{"program"}},
      {"additionalProperties", false},
  };
}
