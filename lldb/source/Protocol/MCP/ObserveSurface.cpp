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

// A schema's job is to make a call valid: the names, the types, the enumerated
// values and what is required. That is what these helpers always emit, and a
// description is what they take only when there is something to say that the
// name and the type do not already say.
//
// Prose arguing *why* to set a field is discovery text, and a caller reading the
// schema has already decided to call the tool -- it is filling the field in. What
// it was reading instead: of 6,352 wire bytes of `inputSchema`, the contents of
// 30 description strings were 4,299 and their `"description":` wrappers another
// 510, leaving 1,543 for every type, enum, default and key together. So a reader
// looking for the shape of a call was reading five parts prose to one part shape,
// and some of that prose only restated the field's own name -- "args":
// "Arguments passed to the program."
//
// An `enum` or a `default` earns its bytes differently and both are kept
// everywhere they apply: they are short, they are machine-checkable, and a
// default is the one thing that tells a caller not to set the field at all.

json::Value schemaField(StringRef type) {
  return json::Object{{"type", type}};
}

json::Value schemaField(StringRef type, StringRef description) {
  return json::Object{{"type", type}, {"description", description}};
}

json::Value schemaStringArray() {
  return json::Object{{"type", "array"},
                      {"items", json::Object{{"type", "string"}}}};
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

json::Value schemaNumber(int64_t fallback) {
  return json::Object{{"type", "integer"}, {"default", fallback}};
}

json::Value schemaNumber(int64_t fallback, StringRef description) {
  return json::Object{
      {"type", "integer"}, {"default", fallback}, {"description", description}};
}

json::Value schemaStringMap() {
  return json::Object{
      {"type", "object"},
      {"additionalProperties", json::Object{{"type", "string"}}}};
}

} // namespace

json::Value lldb_protocol::mcp::ObservationSchema() {
  return json::Object{
      {"type", "object"},
      {"properties",
       json::Object{
           {"at", schemaField("string",
                              "A function name, qualified or not, or a source "
                              "location written as \"file.cpp:1189\". Not an "
                              "address.")},
           {"label", schemaField("string", "Defaults to \"at\".")},
           {"on", schemaEnum(json::Array{"entry", "return"}, "entry",
                             "At \"return\" the frame is gone: capture "
                             "\"$return\", not the function's own names.")},
           {"capture",
            schemaStringArray(
                "Expressions read at each hit, in the program's language: a "
                "path like \"tok->text\", or a call like \"tok->dump()\". A "
                "path names what the type declares, so the field behind an "
                "accessor rather than the accessor.")},
           {"when",
            schemaField("string", "A false condition still counts as a hit, "
                                  "but is neither captured nor emitted.")},
           {"called_from", schemaField("string")},
           {"enabled_after",
            schemaField("string", "Held disabled until the observation with "
                                  "this label has been hit.")},
           {"skip_first", schemaNumber(0)},
           {"only_hit",
            schemaField("integer", "Records a single hit, counting from one.")},
           {"emit",
            schemaEnum(json::Array{"every_hit", "on_change", "first_and_last"},
                       "every_hit",
                       "Which hits reach the event stream; aggregation covers "
                       "every hit regardless.")},
           {"backtrace", schemaNumber(0)},
           {"depth", schemaNumber(2)},
       }},
      {"required", json::Array{"at"}},
      {"additionalProperties", false},
  };
}

json::Value lldb_protocol::mcp::ObservationPlanSchema() {
  return json::Object{
      {"type", "object"},
      {"properties",
       json::Object{
           {"program", schemaField("string", "The path to the program to "
                                             "run.")},
           {"args", schemaStringArray()},
           {"env",
            json::Object{
                {"type", "object"},
                {"additionalProperties", json::Object{{"type", "string"}}},
                {"description",
                 "Added to the environment the program would otherwise "
                 "inherit. Values must be strings."}}},
           {"cwd", schemaField("string")},
           {"stdin",
            schemaField("string", "A path whose contents are fed to the "
                                  "program's standard input.")},
           {"capture_inferior_output",
            json::Object{{"type", "boolean"}, {"default", true}}},
           {"fast",
            json::Object{{"type", "boolean"},
                         {"default", true},
                         {"description",
                          "Compile a tracepoint's condition into the program. "
                          "Off recompiles nothing, at a stop per hit; turn it "
                          "off when the program's own timing is what is under "
                          "investigation."}}},
           {"timeout_seconds",
            schemaNumber(30, "Ceiling on the program, from the launch. "
                             "Reaching it is a result, \"timed_out\", not an "
                             "error.")},
           {"no_progress_seconds",
            schemaField("integer",
                        "Give up after this long with no tracepoint hit at "
                        "all. Must be under \"timeout_seconds\"; absent "
                        "disarms it.")},
           {"observe",
            json::Object{{"type", "array"},
                         {"items", ObservationSchema()},
                         {"description",
                          "The tracepoints. Absent or empty runs the program "
                          "and reports only how it ended."}}},
           {"compare",
            json::Object{
                {"type", "array"},
                {"items",
                 json::Object{
                     {"type", "object"},
                     {"properties",
                      json::Object{
                          {"label", schemaField("string")},
                          {"program", schemaField("string")},
                          {"args", schemaStringArray()},
                          {"env", schemaStringMap()},
                          {"cwd", schemaField("string")},
                          {"stdin", schemaField("string")},
                      }},
                     {"required", json::Array{"label"}},
                     {"additionalProperties", false},
                 }},
                {"description",
                 "Runs to make and compare, each this plan with a few fields "
                 "replaced. The response is the differences, not one report "
                 "per run."}}},
       }},
      {"required", json::Array{"program"}},
      {"additionalProperties", false},
  };
}

json::Value
lldb_protocol::mcp::ObserveInputSchema(StringRef debugger_description) {
  return json::Object{
      {"type", "object"},
      {"properties",
       json::Object{
           {"plan", ObservationPlanSchema()},
           {"debugger", schemaField("string", debugger_description)},
       }},
      {"required", json::Array{"plan"}},
      {"additionalProperties", false},
  };
}

