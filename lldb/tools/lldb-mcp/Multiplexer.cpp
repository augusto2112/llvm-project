//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Multiplexer.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include <map>
#include <memory>

using namespace llvm;
using namespace lldb_protocol::mcp;
using namespace lldb_mcp;

namespace {

/// Server name reported to the client during initialization.
constexpr llvm::StringLiteral kServerName = "lldb-mcp";

/// Client-facing tool names.
/// @{
constexpr llvm::StringLiteral kToolCommand = "command";
constexpr llvm::StringLiteral kToolObserve = "observe";
constexpr llvm::StringLiteral kToolSessionsList = "sessions_list";
constexpr llvm::StringLiteral kToolSessionCreate = "session_create";
constexpr llvm::StringLiteral kToolSessionClose = "session_close";
/// @}

/// Backend tool names, as exposed by the LLDB MCP server.
/// @{
constexpr llvm::StringLiteral kBackendToolCommand = "command";
constexpr llvm::StringLiteral kBackendToolObserve = "observe";
constexpr llvm::StringLiteral kBackendToolDebuggerList = "debugger_list";
constexpr llvm::StringLiteral kBackendToolDebuggerCreate = "debugger_create";
constexpr llvm::StringLiteral kBackendToolDebuggerDelete = "debugger_delete";
/// @}

/// Backend-local URI prefixes.
/// @{
constexpr llvm::StringLiteral kDebuggerLocalPrefix = "lldb-mcp://debugger/";
constexpr llvm::StringLiteral kResourceLocalPrefix = "lldb://debugger/";
/// @}

std::string replaceAll(StringRef text, StringRef from, StringRef to) {
  std::string result;
  size_t pos = 0;
  while (true) {
    size_t next = text.find(from, pos);
    if (next == StringRef::npos) {
      result += text.substr(pos).str();
      break;
    }
    result += text.substr(pos, next - pos).str();
    result += to.str();
    pos = next + from.size();
  }
  return result;
}

CallToolResult makeTextResult(std::string text) {
  CallToolResult result;
  result.content.emplace_back(TextContent{{std::move(text)}});
  return result;
}

/// The description of the `observe` tool as a client reads it.
///
/// This and observeInputSchema below duplicate lldb_private::mcp::ObserveTool,
/// which is what an LLDB MCP server serves for the same tool. The multiplexer
/// owns the client-facing surface and answers tools/list itself, without
/// reaching into the plugin, so the two copies have to be kept in step. Only
/// the `debugger` field differs, because a client sees instance-qualified URIs
/// and a backend does not.
constexpr llvm::StringLiteral kObserveDescription =
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
    "set to the hit number it named, which records that one hit in full "
    "detail.";

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

/// The schema of the plan, which carries no MCP arguments of its own and so is
/// nested under "plan" rather than flattened into the tool's arguments.
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

json::Value observeInputSchema() {
  return json::Object{
      {"type", "object"},
      {"properties",
       json::Object{
           {"plan", planSchema()},
           {"debugger",
            schemaField(
                "string",
                "The debugger URI selecting the debug session to run in, as "
                "reported by sessions_list, e.g. "
                "lldb-mcp://instance/{pid}/debugger/{id}. Defaults to the "
                "local session.")},
       }},
      {"required", json::Array{"plan"}},
      {"additionalProperties", false},
  };
}

} // namespace

std::string lldb_mcp::RewriteURIsToGlobal(StringRef text, lldb::pid_t pid) {
  std::string debugger_global =
      formatv("lldb-mcp://instance/{0}/debugger/", pid).str();
  std::string resource_global =
      formatv("lldb://instance/{0}/debugger/", pid).str();
  std::string step = replaceAll(text, kDebuggerLocalPrefix, debugger_global);
  return replaceAll(step, kResourceLocalPrefix, resource_global);
}

std::optional<RoutedURI> lldb_mcp::ParseGlobalURI(StringRef uri) {
  size_t scheme_end = uri.find("://");
  if (scheme_end == StringRef::npos)
    return std::nullopt;

  StringRef scheme = uri.take_front(scheme_end);
  StringRef rest = uri.drop_front(scheme_end + 3);
  if (!rest.consume_front("instance/"))
    return std::nullopt;

  size_t slash = rest.find('/');
  StringRef pid_str = slash == StringRef::npos ? rest : rest.take_front(slash);
  lldb::pid_t pid;
  if (pid_str.getAsInteger(10, pid))
    return std::nullopt;

  StringRef tail =
      slash == StringRef::npos ? StringRef() : rest.drop_front(slash + 1);
  RoutedURI routed;
  routed.pid = pid;
  routed.local = formatv("{0}://{1}", scheme, tail).str();
  return routed;
}

Multiplexer::Multiplexer(std::unique_ptr<MCPTransport> client_transport,
                         LogCallback log_callback)
    : m_client_transport(std::move(client_transport)),
      m_client_binder(std::make_unique<MCPBinder>(*m_client_transport)),
      m_log_callback(std::move(log_callback)) {
  m_client_binder->Bind<InitializeResult, InitializeParams>(
      "initialize", &Multiplexer::HandleInitialize, this);
  m_client_binder->Bind<ListToolsResult, void>(
      "tools/list", &Multiplexer::HandleToolsList, this);
  m_client_binder->BindAsync<CallToolResult, CallToolParams>(
      "tools/call", &Multiplexer::HandleToolsCall, this);
  m_client_binder->BindAsync<ListResourcesResult, void>(
      "resources/list", &Multiplexer::HandleResourcesList, this);
  m_client_binder->BindAsync<ReadResourceResult, ReadResourceParams>(
      "resources/read", &Multiplexer::HandleResourcesRead, this);
  m_client_binder->Bind<void>("notifications/initialized", [this]() {
    Log("client initialization complete");
  });
}

void Multiplexer::InstallBackendHandlers(lldb::pid_t pid, Client &backend) {
  backend.SetDisconnectHandler([this, pid]() { RetireBackend(pid); });
  backend.SetErrorHandler([this, pid](llvm::Error error) {
    consumeError(std::move(error));
    RetireBackend(pid);
  });
}

void Multiplexer::AddBackend(lldb::pid_t pid, std::unique_ptr<Client> backend) {
  InstallBackendHandlers(pid, *backend);
  m_backends.push_back(Backend{pid, std::move(backend), /*local=*/false});
}

void Multiplexer::AddLocalBackend(lldb::pid_t pid,
                                  std::unique_ptr<Client> backend) {
  InstallBackendHandlers(pid, *backend);
  m_backends.push_back(Backend{pid, std::move(backend), /*local=*/true});
}

llvm::Error Multiplexer::Run() {
  // Run is called only after a backend has been added. Starting with none is a
  // setup bug, not a usable server.
  if (m_backends.empty())
    return llvm::createStringError(
        "no backends registered before Multiplexer::Run");

  m_client_binder->OnDisconnect(&Multiplexer::HandleDisconnect, this);
  m_client_binder->OnError([this](llvm::Error err) {
    Log(formatv("client transport error: {0}", toString(std::move(err))).str());
  });
  return m_client_transport->RegisterMessageHandler(*m_client_binder);
}

void Multiplexer::Shutdown() {
  for (Backend &backend : m_backends)
    backend.client->CancelPendingRequests("lldb-mcp is shutting down");
}

void Multiplexer::SetDisconnectHandler(llvm::unique_function<void()> handler) {
  m_disconnect_handler = std::move(handler);
}

Client *Multiplexer::RouteToPid(lldb::pid_t pid) {
  for (Backend &backend : m_backends)
    if (backend.pid == pid && backend.alive)
      return backend.client.get();
  return nullptr;
}

Multiplexer::Backend *Multiplexer::LocalBackend() {
  for (Backend &backend : m_backends)
    if (backend.local && backend.alive)
      return &backend;
  return nullptr;
}

llvm::SmallVector<Multiplexer::Backend *> Multiplexer::LiveBackends() {
  llvm::SmallVector<Backend *> live;
  for (Backend &backend : m_backends)
    if (backend.alive)
      live.push_back(&backend);
  return live;
}

void Multiplexer::RetireBackend(lldb::pid_t pid) {
  for (Backend &backend : m_backends) {
    if (backend.pid != pid)
      continue;
    if (!backend.alive)
      return;
    backend.alive = false;
    // Fail any in-flight requests so a fanned-out aggregation or a routed call
    // completes with an error instead of hanging.
    backend.client->CancelPendingRequests("backend disconnected");
    return;
  }
}

Expected<InitializeResult>
Multiplexer::HandleInitialize(const InitializeParams &) {
  InitializeResult result;
  result.protocolVersion = kProtocolVersion;
  result.capabilities.supportsToolsList = true;
  result.capabilities.supportsResourcesList = true;
  result.serverInfo.name = kServerName;
  result.serverInfo.version = GetServerVersion();
  return result;
}

Expected<ListToolsResult> Multiplexer::HandleToolsList() {
  ListToolsResult result;

  ToolDefinition command;
  command.name = kToolCommand;
  command.description = "Run an LLDB command in a debug session.";
  command.inputSchema = json::Object{
      {"type", "object"},
      {"properties",
       json::Object{
           {"command",
            json::Object{{"type", "string"},
                         {"description", "The LLDB command to run."}}},
           {"debugger",
            json::Object{
                {"type", "string"},
                {"description",
                 "The debugger URI selecting the debug session, as reported by "
                 "sessions_list, e.g. lldb-mcp://instance/{pid}/debugger/{id}. "
                 "Defaults to the local session."}}},
       }},
      {"required", json::Array{"command"}},
  };
  result.tools.push_back(std::move(command));

  ToolDefinition observe;
  observe.name = kToolObserve;
  observe.description = kObserveDescription;
  observe.inputSchema = observeInputSchema();
  result.tools.push_back(std::move(observe));

  ToolDefinition sessions_list;
  sessions_list.name = kToolSessionsList;
  sessions_list.description =
      "List the active debug sessions across all lldb instances.";
  sessions_list.inputSchema = json::Object{{"type", "object"}};
  result.tools.push_back(std::move(sessions_list));

  ToolDefinition session_create;
  session_create.name = kToolSessionCreate;
  session_create.description =
      "Create a new in-process debug session and return its URI.";
  session_create.inputSchema = json::Object{{"type", "object"}};
  result.tools.push_back(std::move(session_create));

  ToolDefinition session_close;
  session_close.name = kToolSessionClose;
  session_close.description =
      "Close a debug session previously created with session_create.";
  session_close.inputSchema = json::Object{
      {"type", "object"},
      {"properties",
       json::Object{
           {"session", json::Object{{"type", "string"},
                                    {"description",
                                     "The session URI to close, as returned by "
                                     "session_create or sessions_list."}}},
       }},
      {"required", json::Array{"session"}},
  };
  result.tools.push_back(std::move(session_close));

  return result;
}

void Multiplexer::HandleToolsCall(const CallToolParams &params,
                                  Reply<CallToolResult> reply) {
  if (params.name == kToolCommand)
    return HandleRoutedCall(kBackendToolCommand, params, std::move(reply));
  if (params.name == kToolObserve)
    return HandleRoutedCall(kBackendToolObserve, params, std::move(reply));
  if (params.name == kToolSessionsList)
    return HandleSessionsList(std::move(reply));
  if (params.name == kToolSessionCreate)
    return HandleSessionCreate(std::move(reply));
  if (params.name == kToolSessionClose)
    return HandleSessionClose(params, std::move(reply));
  reply(createStringError(formatv("no tool \"{0}\"", params.name)));
}

void Multiplexer::HandleRoutedCall(StringRef backend_tool,
                                   const CallToolParams &params,
                                   Reply<CallToolResult> reply) {
  json::Object args;
  if (params.arguments)
    if (const json::Object *object = params.arguments->getAsObject())
      args = *object;

  std::string debugger_arg;
  if (std::optional<StringRef> debugger = args.getString("debugger"))
    debugger_arg = debugger->str();

  Client *backend = nullptr;
  if (debugger_arg.empty()) {
    // Default to the local session, letting the backend pick its debugger.
    Backend *local = LocalBackend();
    backend = local ? local->client.get() : nullptr;
    args.erase("debugger");
  } else {
    std::optional<RoutedURI> routed = ParseGlobalURI(debugger_arg);
    if (!routed)
      return reply(createStringError(
          formatv("malformed debugger uri \"{0}\"", debugger_arg)));
    backend = RouteToPid(routed->pid);
    args["debugger"] = routed->local;
  }

  if (!backend)
    return reply(createStringError("no debug session available"));

  CallToolParams backend_params;
  backend_params.name = backend_tool;
  backend_params.arguments = json::Value(std::move(args));
  backend->ToolsCall(backend_params, std::move(reply));
}

void Multiplexer::HandleSessionsList(Reply<CallToolResult> reply) {
  llvm::SmallVector<Backend *> live = LiveBackends();
  if (live.empty())
    return reply(makeTextResult(""));

  // All backends share the multiplexer's MainLoop, so these reply callbacks run
  // serially on one thread and the aggregation state below needs no locking.
  struct State {
    size_t remaining;
    // Keyed by pid so the aggregated output is deterministic.
    std::map<lldb::pid_t, std::string> texts;
    Reply<CallToolResult> reply;
  };
  auto state = std::make_shared<State>();
  state->remaining = live.size();
  state->reply = std::move(reply);

  for (Backend *backend : live) {
    lldb::pid_t pid = backend->pid;
    CallToolParams params;
    params.name = kBackendToolDebuggerList;
    backend->client->ToolsCall(
        params, [state, pid](Expected<CallToolResult> result) {
          // Best effort: a backend that fails or disconnected is simply omitted
          // from the aggregate rather than failing the whole listing.
          if (result) {
            std::string text;
            for (const TextContent &content : result->content)
              text += RewriteURIsToGlobal(content.text, pid);
            state->texts[pid] = std::move(text);
          } else {
            consumeError(result.takeError());
          }

          if (--state->remaining != 0)
            return;
          std::string combined;
          for (const auto &entry : state->texts)
            combined += entry.second;
          state->reply(makeTextResult(combined));
        });
  }
}

void Multiplexer::HandleSessionCreate(Reply<CallToolResult> reply) {
  Backend *local = LocalBackend();
  if (!local)
    return reply(createStringError("no in-process session host available"));

  lldb::pid_t pid = local->pid;
  CallToolParams params;
  params.name = kBackendToolDebuggerCreate;
  local->client->ToolsCall(
      params,
      [reply = std::move(reply), pid](Expected<CallToolResult> result) mutable {
        if (!result)
          return reply(result.takeError());
        for (TextContent &content : result->content)
          content.text = RewriteURIsToGlobal(content.text, pid);
        reply(std::move(*result));
      });
}

void Multiplexer::HandleSessionClose(const CallToolParams &params,
                                     Reply<CallToolResult> reply) {
  json::Object args;
  if (params.arguments)
    if (const json::Object *object = params.arguments->getAsObject())
      args = *object;

  std::optional<StringRef> session = args.getString("session");
  if (!session || session->empty())
    return reply(createStringError("session_close requires a \"session\" uri"));

  std::optional<RoutedURI> routed = ParseGlobalURI(*session);
  if (!routed)
    return reply(
        createStringError(formatv("malformed session uri \"{0}\"", *session)));

  Backend *local = LocalBackend();
  if (!local || routed->pid != local->pid)
    return reply(
        createStringError("can only close sessions that lldb-mcp created"));

  CallToolParams delete_params;
  delete_params.name = kBackendToolDebuggerDelete;
  delete_params.arguments = json::Object{{"debugger", routed->local}};
  local->client->ToolsCall(delete_params, std::move(reply));
}

void Multiplexer::HandleResourcesList(Reply<ListResourcesResult> reply) {
  llvm::SmallVector<Backend *> live = LiveBackends();
  if (live.empty())
    return reply(ListResourcesResult{});

  // These reply callbacks run serially on the shared MainLoop thread, so this
  // state needs no locking.
  struct State {
    size_t remaining;
    std::map<lldb::pid_t, std::vector<Resource>> resources;
    Reply<ListResourcesResult> reply;
  };
  auto state = std::make_shared<State>();
  state->remaining = live.size();
  state->reply = std::move(reply);

  for (Backend *backend : live) {
    lldb::pid_t pid = backend->pid;
    backend->client->ResourcesList(
        [state, pid](Expected<ListResourcesResult> result) {
          // Best effort: a failed or disconnected backend contributes no
          // resources rather than failing the whole listing.
          if (result) {
            std::vector<Resource> rewritten;
            for (Resource resource : result->resources) {
              resource.uri = RewriteURIsToGlobal(resource.uri, pid);
              rewritten.push_back(std::move(resource));
            }
            state->resources[pid] = std::move(rewritten);
          } else {
            consumeError(result.takeError());
          }

          if (--state->remaining != 0)
            return;
          ListResourcesResult combined;
          for (auto &entry : state->resources)
            for (Resource &resource : entry.second)
              combined.resources.push_back(std::move(resource));
          state->reply(std::move(combined));
        });
  }
}

void Multiplexer::HandleResourcesRead(const ReadResourceParams &params,
                                      Reply<ReadResourceResult> reply) {
  std::optional<RoutedURI> routed = ParseGlobalURI(params.uri);
  if (!routed)
    return reply(createStringError(
        formatv("malformed resource uri \"{0}\"", params.uri)));

  Client *backend = RouteToPid(routed->pid);
  if (!backend)
    return reply(createStringError(formatv("no instance {0}", routed->pid)));

  ReadResourceParams backend_params;
  backend_params.uri = routed->local;
  lldb::pid_t pid = routed->pid;
  backend->ResourcesRead(
      backend_params, [reply = std::move(reply),
                       pid](Expected<ReadResourceResult> result) mutable {
        if (result)
          for (TextResourceContents &content : result->contents)
            content.uri = RewriteURIsToGlobal(content.uri, pid);
        reply(std::move(result));
      });
}

void Multiplexer::HandleDisconnect() {
  if (m_disconnect_handler)
    m_disconnect_handler();
}

void Multiplexer::Log(llvm::StringRef message) {
  if (m_log_callback)
    m_log_callback(message);
}
