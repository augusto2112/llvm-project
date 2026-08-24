//===- Tool.cpp -----------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Tool.h"
#include "ObservationEngine.h"
#include "ObservationPlan.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Host/File.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Protocol/MCP/Protocol.h"
#include "lldb/Utility/FileSpec.h"
#include "lldb/Utility/UriParser.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

using namespace lldb_private;
using namespace lldb_protocol;
using namespace lldb_private::mcp;
using namespace lldb;
using namespace llvm;

namespace {

static constexpr StringLiteral kSchemeAndHost = "lldb-mcp://debugger/";

struct CommandToolArguments {
  /// Either an id like '1' or a uri like 'lldb-mcp://debugger/1'.
  std::string debugger;
  std::string command;
};

bool fromJSON(const json::Value &V, CommandToolArguments &A, json::Path P) {
  json::ObjectMapper O(V, P);
  return O && O.mapOptional("debugger", A.debugger) &&
         O.mapOptional("command", A.command);
}

/// Helper function to create a CallToolResult from a string output.
static lldb_protocol::mcp::CallToolResult
createTextResult(std::string output, bool is_error = false) {
  lldb_protocol::mcp::CallToolResult text_result;
  text_result.content.emplace_back(
      lldb_protocol::mcp::TextContent{{std::move(output)}});
  text_result.isError = is_error;
  return text_result;
}

std::string to_uri(DebuggerSP debugger) {
  return (kSchemeAndHost + std::to_string(debugger->GetID())).str();
}

/// Resolves the debugger an MCP `debugger` argument names, which is either an
/// id like '1' or a uri like 'lldb-mcp://debugger/1'. An empty specifier takes
/// the first debugger there is, so that a client driving a single session does
/// not have to name it.
Expected<DebuggerSP> findDebugger(StringRef specifier) {
  if (specifier.empty()) {
    for (size_t i = 0; i < Debugger::GetNumDebuggers(); i++)
      if (DebuggerSP debugger_sp = Debugger::GetDebuggerAtIndex(i))
        return debugger_sp;
    return createStringError("no debugger found");
  }

  StringRef id = specifier;
  id.consume_front(kSchemeAndHost);
  uint32_t debugger_id = 0;
  if (id.consumeInteger(10, debugger_id))
    return createStringError(
        formatv("malformed debugger specifier {0}", specifier));

  DebuggerSP debugger_sp = Debugger::FindDebuggerWithID(debugger_id);
  if (!debugger_sp)
    return createStringError("no debugger found");
  return debugger_sp;
}

} // namespace

Expected<lldb_protocol::mcp::CallToolResult>
CommandTool::Call(const lldb_protocol::mcp::ToolArguments &args) {
  if (!std::holds_alternative<json::Value>(args))
    return createStringError("CommandTool requires arguments");

  json::Path::Root root;

  CommandToolArguments arguments;
  if (!fromJSON(std::get<json::Value>(args), arguments, root))
    return root.getError();

  Expected<DebuggerSP> debugger_sp = findDebugger(arguments.debugger);
  if (!debugger_sp)
    return debugger_sp.takeError();

  // FIXME: Disallow certain commands and their aliases.
  CommandReturnObject result(/*colors=*/false);
  (*debugger_sp)
      ->GetCommandInterpreter()
      .HandleCommand(arguments.command.c_str(), eLazyBoolYes, result);

  std::string output;
  StringRef output_str = result.GetOutputString();
  if (!output_str.empty())
    output += output_str.str();

  std::string err_str = result.GetErrorString();
  if (!err_str.empty()) {
    if (!output.empty())
      output += '\n';
    output += err_str;
  }

  return createTextResult(output, !result.Succeeded());
}

std::optional<json::Value> CommandTool::GetSchema() const {
  using namespace llvm::json;
  Object properties{
      {"debugger",
       Object{{"type", "string"},
              {"description",
               "The debugger ID or URI to a specific debug session. If not "
               "specified, the first debugger will be used."}}},
      {"command",
       Object{{"type", "string"}, {"description", "An lldb command to run."}}}};
  Object schema{{"type", "object"}, {"properties", std::move(properties)}};
  return schema;
}

//===----------------------------------------------------------------------===//
// ObserveTool
//===----------------------------------------------------------------------===//

const StringLiteral lldb_private::mcp::ObserveToolDescription =
    "Run a program under a set of tracepoints and report what happened, "
    "instead of stepping through it. One call launches the program, reads the "
    "expressions named at each tracepoint, lets the program run to its own "
    "end, and comes back with a summary over every hit plus a JSONL artifact "
    "holding the full event stream.\n"
    "\n"
    "The plan is the \"plan\" argument. \"debugger\" sits beside it, outside "
    "the plan, because it selects the session to run in rather than describing "
    "the run.\n"
    "\n"
    "An empty \"observe\" list is crash triage: the program runs untouched, "
    "and the result is how it ended, with a ranked backtrace, locals and "
    "source at the failure.\n"
    "\n"
    "Prefer a capture that is a path, \"I.Ty.TypeID\", over one that is a "
    "call, \"I->getType()\"; \"->\" and \"[]\" are part of a path. A path is a "
    "debug-info lookup and a memory read, while a call compiles and runs code "
    "inside the observed process. On a hot tracepoint a call is measured and "
    "turned off partway through the run to keep the run inside its timeout, so "
    "it yields partial data where the equivalent path would have yielded all "
    "of it.\n"
    "\n"
    "Capture more expressions than you think you need. A capture costs wall "
    "clock once per run, not tokens per round trip, and the alternative to "
    "capturing it now is running the whole program again to ask one more "
    "question.\n"
    "\n"
    "Read \"aggregate\" first. Its \"outliers\", the values seen once or twice "
    "among many hits, are usually the answer. Then re-run with \"only_hit\" "
    "set to that outlier's \"first_hit\", which records that one hit in full "
    "detail.\n"
    "\n"
    "An outlier carries two numbers because they count different things. "
    "\"first_hit\" counts that observation's own hits and is what \"only_hit\" "
    "takes. \"first_seq\" numbers the whole event stream and is what matches a "
    "line in the artifact. They are equal only when a plan holds a single "
    "observation.";

namespace {

/// The arguments `observe` accepts. Everything describing the run lives inside
/// the plan, so this list stays two entries long however the plan grows.
constexpr StringRef kObserveArguments[] = {"debugger", "plan"};

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

/// The schema of one entry in the plan's "observe" list.
json::Value observationSchema() {
  return json::Object{
      {"type", "object"},
      {"properties",
       json::Object{
           {"at",
            schemaField("string",
                        "What to observe: either a function name, or a source "
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
                             "rather than as it is entered.")},
           {"capture",
            schemaStringArray(
                "Expressions to read at each hit. Empty is a bare tracepoint "
                "recording only hit counts, which already answers whether the "
                "code runs at all.")},
           {"when",
            schemaField("string",
                        "A condition evaluated at each hit. A hit whose "
                        "condition is false still counts as a hit, but is "
                        "neither captured nor emitted.")},
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

/// The schema of the plan itself, which is what ParseObservationPlan accepts.
/// It carries no MCP arguments, so it is nested under "plan" rather than
/// flattened into the tool's arguments.
json::Value planSchema() {
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
            schemaNumber(30, "Wall-clock ceiling on the whole run. A run that "
                             "never terminates is a result rather than a "
                             "failure, so there is always a limit.")},
           {"no_progress_seconds",
            schemaField("integer",
                        "Gives up after this long with no emitted event, and "
                        "must be shorter than \"timeout_seconds\". Absent "
                        "leaves the check disarmed, because a plan whose "
                        "triggers only fire near the end of a run is "
                        "legitimate and would otherwise be cut short.")},
           {"observe",
            json::Object{
                {"type", "array"},
                {"items", observationSchema()},
                {"description",
                 "The tracepoints. An absent or empty list is a legal plan: it "
                 "runs the program and reports only how it ended, which is "
                 "crash triage."}}},
       }},
      {"required", json::Array{"program"}},
      {"additionalProperties", false},
  };
}

} // namespace

std::optional<json::Value> ObserveTool::GetSchema() const {
  return json::Object{
      {"type", "object"},
      {"properties",
       json::Object{
           {"plan", planSchema()},
           {"debugger",
            schemaField("string",
                        "The debugger ID or URI of the session to run in. If "
                        "not specified, the first debugger will be used.")},
       }},
      {"required", json::Array{"plan"}},
      {"additionalProperties", false},
  };
}

Expected<lldb_protocol::mcp::CallToolResult>
ObserveTool::Call(const lldb_protocol::mcp::ToolArguments &args) {
  if (!std::holds_alternative<json::Value>(args))
    return createStringError("ObserveTool requires arguments");

  const json::Object *arguments = std::get<json::Value>(args).getAsObject();
  if (!arguments)
    return createStringError(
        "observe: the arguments must be an object carrying \"plan\", the "
        "observation plan to run.");

  const json::Value *plan_value = arguments->get("plan");
  if (!plan_value) {
    // Writing the plan's own fields as the arguments is the mistake the shape
    // invites, so it is answered with the fix rather than with a list of
    // rejected fields.
    if (arguments->get("program") || arguments->get("observe"))
      return createStringError(
          "observe: the plan goes inside \"plan\", not beside it. Wrap what "
          "you passed: {\"plan\": {\"program\": \"...\", \"observe\": [...]}}. "
          "Only \"debugger\" stays outside the plan, because it selects the "
          "session rather than describing the run.");
    return createStringError(
        "observe: \"plan\" is required. It is an observation plan, whose only "
        "required field is \"program\", the path to the program to run; the "
        "\"observe\" list of tracepoints is optional, and leaving it out runs "
        "the program and reports only how it ended.");
  }

  SmallVector<StringRef, 2> unknown;
  for (const auto &argument : *arguments)
    if (!is_contained(kObserveArguments, StringRef(argument.first)))
      unknown.push_back(argument.first);
  if (!unknown.empty()) {
    // Object iteration order is unspecified, so the message is sorted to keep
    // it reproducible for the same arguments.
    llvm::sort(unknown);
    return createStringError(
        formatv("observe: unrecognized argument{0} \"{1}\". The arguments are "
                "\"plan\" and \"debugger\"; every field describing the run "
                "belongs inside \"plan\".",
                unknown.size() == 1 ? "" : "s", join(unknown, "\", \"")));
  }

  Expected<ObservationPlan> plan = ParseObservationPlan(*plan_value);
  if (!plan)
    return plan.takeError();

  std::string debugger_argument;
  if (std::optional<StringRef> debugger = arguments->getString("debugger"))
    debugger_argument = debugger->str();

  Expected<DebuggerSP> debugger_sp = findDebugger(debugger_argument);
  if (!debugger_sp)
    return debugger_sp.takeError();

  ObservationEngine engine(**debugger_sp, std::move(*plan));
  Expected<ObservationResult> result = engine.Run();
  if (!result)
    return result.takeError();

  // A run that crashed, hung or observed nothing is the answer rather than a
  // failure, so the outcome is reported in the document and not as an error.
  std::string output;
  raw_string_ostream os(output);
  os << result->Render();
  return createTextResult(std::move(output));
}

Expected<lldb_protocol::mcp::CallToolResult>
DebuggerListTool::Call(const lldb_protocol::mcp::ToolArguments &args) {
  llvm::json::Path::Root root;

  // Return a nested Markdown list with debuggers and target.
  // Example output:
  //
  // - lldb-mcp://debugger/1
  // - lldb-mcp://debugger/2
  //
  // FIXME: Use Structured Content when we adopt protocol version 2025-06-18.
  std::string output;
  llvm::raw_string_ostream os(output);

  const size_t num_debuggers = Debugger::GetNumDebuggers();
  for (size_t i = 0; i < num_debuggers; ++i) {
    lldb::DebuggerSP debugger_sp = Debugger::GetDebuggerAtIndex(i);
    if (!debugger_sp)
      continue;

    os << "- " << to_uri(debugger_sp) << '\n';
  }

  return createTextResult(output);
}

/// Opens the platform null device with the given options, or nullptr on error.
static lldb::FileSP openNull(File::OpenOptions options) {
  llvm::Expected<lldb::FileUP> file =
      FileSystem::Instance().Open(FileSpec(FileSystem::DEV_NULL), options);
  if (!file) {
    llvm::consumeError(file.takeError());
    return nullptr;
  }
  return std::move(*file);
}

Expected<lldb_protocol::mcp::CallToolResult>
DebuggerCreateTool::Call(const lldb_protocol::mcp::ToolArguments &) {
  // Redirect the new debugger's stdio to the null device so its prompt and
  // async output can't corrupt an MCP stream sharing the host's stdout. Command
  // results flow through CommandReturnObject and are unaffected. Open the null
  // files first so a failure can't leave a created debugger on the real stdio.
  // The single write-only null file backs both stdout and stderr.
  lldb::FileSP in = openNull(File::eOpenOptionReadOnly);
  lldb::FileSP out = openNull(File::eOpenOptionWriteOnly);
  if (!in || !out)
    return createStringError(
        "failed to open the null device for debugger stdio");

  lldb::DebuggerSP debugger_sp = Debugger::CreateInstance();
  if (!debugger_sp)
    return createStringError("failed to create debugger");

  debugger_sp->SetInputFile(in);
  debugger_sp->SetOutputFile(out);
  debugger_sp->SetErrorFile(out);

  // A debugger driven over MCP has no event loop to service asynchronous
  // stops, so a resume must not return before the process has stopped.
  debugger_sp->SetAsyncExecution(false);

  return createTextResult(to_uri(debugger_sp));
}

Expected<lldb_protocol::mcp::CallToolResult>
DebuggerDeleteTool::Call(const lldb_protocol::mcp::ToolArguments &args) {
  if (!std::holds_alternative<json::Value>(args))
    return createStringError("DebuggerDeleteTool requires arguments");

  const json::Object *arguments = std::get<json::Value>(args).getAsObject();
  if (!arguments)
    return createStringError("DebuggerDeleteTool requires arguments");

  std::optional<StringRef> debugger = arguments->getString("debugger");
  if (!debugger)
    return createStringError("DebuggerDeleteTool requires a debugger");

  StringRef specifier = *debugger;
  specifier.consume_front(kSchemeAndHost);
  uint32_t debugger_id = 0;
  if (specifier.consumeInteger(10, debugger_id))
    return createStringError(
        formatv("malformed debugger specifier {0}", *debugger));

  lldb::DebuggerSP debugger_sp = Debugger::FindDebuggerWithID(debugger_id);
  if (!debugger_sp)
    return createStringError("no debugger found");

  Debugger::Destroy(debugger_sp);
  return createTextResult(formatv("deleted {0}", *debugger).str());
}

std::optional<json::Value> DebuggerDeleteTool::GetSchema() const {
  using namespace llvm::json;
  Object properties{
      {"debugger",
       Object{{"type", "string"},
              {"description", "The debugger ID or URI to destroy."}}}};
  Object schema{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", Array{"debugger"}}};
  return schema;
}
