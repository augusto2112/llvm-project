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
#include "lldb/Target/Process.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/TargetList.h"
#include "lldb/Utility/State.h"
#include "lldb/Host/File.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Protocol/MCP/ObserveSurface.h"
#include "lldb/Protocol/MCP/Protocol.h"
#include "lldb/Utility/FileSpec.h"
#include "lldb/Utility/UriParser.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include <atomic>
#include <chrono>
#include <thread>
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
    // Distinct from the message a named-but-missing debugger gets below. Here
    // there is no session at all, and the caller has not said which one it
    // wanted, so the fix is a step it has not taken rather than a bad argument.
    return createStringError(
        "no debug session exists yet: call session_create to open one, or pass "
        "\"debugger\" with a uri from sessions_list");
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

/// Creates a debugger for MCP to drive, with the stdio and the execution mode a
/// client needs rather than the ones an interactive lldb needs.
static Expected<DebuggerSP> createManagedDebugger() {
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
  return debugger_sp;
}

/// How long one command may block before the session is interrupted and the
/// caller told so.
///
/// A command that waits for a program to stop cannot return for a program that
/// never does. `process launch` in a synchronous session waits for the first stop,
/// and a program launched with no breakpoints in it has none: measured on a
/// compiler hang reached through this tool, the call blocked until the client
/// abandoned it after thirty minutes, and the follow-up `process interrupt` --
/// which needs an event loop this session does not have -- blocked as well. Half of
/// that cell's wall clock went on two calls that could never answer. A ceiling
/// makes the answer wrong-but-honest instead of absent, and leaves the process
/// stopped where it had got to, which is a session the caller can go on using.
static constexpr std::chrono::seconds CommandCeiling{120};

/// Whether \p debugger holds a target with a process that exists, launched or
/// attached. Read before and after a command to tell whether it started one.
static bool hasLiveProcess(Debugger &debugger) {
  lldb::TargetSP target = debugger.GetTargetList().GetSelectedTarget();
  if (!target)
    return false;
  lldb::ProcessSP process = target->GetProcessSP();
  return process && process->IsAlive();
}

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

  // Whether a process existed before the command ran, so that a launch can be
  // recognised by what it did rather than by how it was spelled. Every alias and
  // abbreviation of `run` and `process launch` reaches this the same way, and a
  // command that merely resumed one does not.
  const bool had_process = hasLiveProcess(**debugger_sp);

  // FIXME: Disallow certain commands and their aliases.
  CommandReturnObject result(/*colors=*/false);
  std::atomic<bool> interrupted{false};
  {
    // Interrupted from a second thread, which is where the command interpreter
    // expects an interrupt to come from: it is what Ctrl-C is in the driver. The
    // halt is what actually ends a wait for a stop, and the interrupt request is
    // what ends the commands that poll instead of waiting.
    std::atomic<bool> Finished{false};
    Debugger &Dbg = **debugger_sp;
    std::thread Watchdog([&] {
      const auto Deadline = std::chrono::steady_clock::now() + CommandCeiling;
      while (!Finished.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= Deadline)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (Finished.load(std::memory_order_acquire))
        return;
      // Recorded here rather than read back from the debugger afterwards:
      // interrupts are documented as inactive on the command-interpreter thread,
      // which is the thread that would be asking.
      interrupted = true;

      // Kept up until the command returns, rather than tried once. A program
      // under a breakpoint with a large ignore count spends most of its time
      // stopped -- it is the stop processing that resumes it -- so a single look
      // finds it stopped, sends nothing, and lets it run on: measured on a
      // `continue` past an ignore count of two million, which went on for the
      // 1800 seconds the client allowed with a watchdog that had already fired.
      // A halt only has an effect on a running process, so the retry is what
      // catches one of the moments it is.
      while (!Finished.load(std::memory_order_acquire)) {
        Dbg.RequestInterrupt();
        if (lldb::TargetSP Target = Dbg.GetTargetList().GetSelectedTarget())
          if (lldb::ProcessSP Process = Target->GetProcessSP())
            if (Process->IsAlive() && StateIsRunningState(Process->GetState()))
              Process->Halt(/*clear_thread_plans=*/false);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });

    Dbg.GetCommandInterpreter().HandleCommand(arguments.command.c_str(),
                                              eLazyBoolYes, result);
    Finished.store(true, std::memory_order_release);
    Watchdog.join();
    Dbg.CancelInterruptRequest();
  }

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

  // A command the ceiling ended is reported as one, because the alternative is a
  // caller reading a truncated or empty result as the answer.
  if (interrupted) {
    if (!output.empty())
      output += '\n';
    output += formatv(
        "\nnote: this command was still waiting after {0}s and the session was "
        "interrupted; the program is stopped where it had got to, and this session "
        "is usable. A command that waits for the program to stop cannot return for "
        "a program that does not: to run one to its end and get a summary of what "
        "happened while it ran, call \"trace_program\" with a plan.",
        CommandCeiling.count());
  }

  // Said here rather than in the description, because the description does not
  // work, and not when the note above already said more of it. Measured on an agent that had just loaded this tool's description --
  // which says in as many words to use `trace_program` to run a program -- and
  // then set `target.run-args` by hand, fought the quoting of them, and launched
  // anyway. A tool that matches what the caller already believes a debugger is will
  // be reached for over one that does not, and the only place left to say so is at
  // the moment of the launch.
  if (!interrupted && !had_process && hasLiveProcess(**debugger_sp)) {
    if (!output.empty())
      output += '\n';
    output +=
        "\nnote: this session now holds a launched process, which \"command\" "
        "drives one stop at a time. To run a program to its end and get a summary "
        "over every hit of a set of tracepoints, the values at each, where it got "
        "stuck and where sampled stacks found it, call \"trace_program\" with a "
        "plan instead -- it does the launch, the arguments and the collection in "
        "one call.";
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

namespace {

/// The arguments `observe` accepts. Everything describing the run lives inside
/// the plan, so this list stays two entries long however the plan grows.
constexpr StringRef kObserveArguments[] = {"debugger", "plan"};

json::Value schemaField(StringRef type, StringRef description) {
  return json::Object{{"type", type}, {"description", description}};
}

} // namespace

std::optional<json::Value> ObserveTool::GetSchema() const {
  return json::Object{
      {"type", "object"},
      {"properties",
       json::Object{
           {"plan", lldb_protocol::mcp::ObservationPlanSchema()},
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
        "trace_program: the arguments must be an object carrying \"plan\", the "
        "observation plan to run.");

  const json::Value *plan_value = arguments->get("plan");
  if (!plan_value) {
    // Writing the plan's own fields as the arguments is the mistake the shape
    // invites, so it is answered with the fix rather than with a list of
    // rejected fields.
    if (arguments->get("program") || arguments->get("observe"))
      return createStringError(
          "trace_program: the plan goes inside \"plan\", not beside it. Wrap what "
          "you passed: {\"plan\": {\"program\": \"...\", \"observe\": [...]}}. "
          "Only \"debugger\" stays outside the plan, because it selects the "
          "session rather than describing the run.");
    return createStringError(
        "trace_program: \"plan\" is required. It is an observation plan, whose only "
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
        formatv("trace_program: unrecognized argument{0} \"{1}\". The arguments are "
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

  // A plan carries the program, its arguments and its environment, so a session
  // adds nothing a caller has to decide: the only thing an empty one is good for
  // is being observed in. Requiring session_create first bought a round trip and
  // a question -- what is in a session, is it reusable, does closing it matter --
  // for a tool whose whole shape is one call per run.
  if (debugger_argument.empty() && Debugger::GetNumDebuggers() == 0)
    if (Expected<DebuggerSP> created = createManagedDebugger();
        !created)
      return created.takeError();

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

Expected<lldb_protocol::mcp::CallToolResult>
DebuggerCreateTool::Call(const lldb_protocol::mcp::ToolArguments &) {
  Expected<DebuggerSP> debugger_sp = createManagedDebugger();
  if (!debugger_sp)
    return debugger_sp.takeError();
  return createTextResult(to_uri(*debugger_sp));
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
