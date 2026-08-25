//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Multiplexer.h"
#include "lldb/Protocol/MCP/ObserveSurface.h"
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

/// The one client-facing tool name.
constexpr llvm::StringLiteral kToolObserve = "trace_program";

/// The backend tool it forwards to, as exposed by the LLDB MCP server. Named
/// separately from the client-facing name because the two are free to diverge:
/// a backend keeps whatever surface its LLDB offers.
constexpr llvm::StringLiteral kBackendToolObserve = "trace_program";

/// Backend-local URI prefixes.
/// @{
constexpr llvm::StringLiteral kDebuggerLocalPrefix = "lldb-mcp://debugger/";
constexpr llvm::StringLiteral kResourceLocalPrefix = "lldb://debugger/";
/// @}

/// What a client is told about `debugger`, which is the one argument whose
/// meaning differs between this server and the plugin behind it: here a session
/// is named by an instance-qualified URI, because there may be several LLDBs.
///
/// It is supplied to the shared schema rather than written into one, so that this
/// string is visibly the one on the wire. It had a twin in the plugin that no
/// client could reach, and the shorter, staler twin was the one a reader found
/// first.
constexpr llvm::StringLiteral kDebuggerDescription =
    "URI of an existing session to run in. Omit it: one is opened as needed.";

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

  // The surface is deliberately a single tool. A tool list is read as a
  // suggestion about how to work, so advertising a session to open, a command
  // to run in it and a session to close would describe the stepping loop
  // `trace_program` replaces, and that is the shape a caller reaches for first.
  ToolDefinition observe;
  observe.name = kToolObserve;
  observe.description = ObserveToolDescription;
  observe.inputSchema = ObserveInputSchema(kDebuggerDescription);
  result.tools.push_back(std::move(observe));

  return result;
}

void Multiplexer::HandleToolsCall(const CallToolParams &params,
                                  Reply<CallToolResult> reply) {
  if (params.name == kToolObserve)
    return HandleRoutedCall(kBackendToolObserve, params, std::move(reply));
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
