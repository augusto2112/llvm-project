//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Multiplexer.h"
#include "TestingSupport/Host/JSONTransportTestUtilities.h"
#include "TestingSupport/SubsystemRAII.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Host/MainLoop.h"
#include "lldb/Host/MainLoopBase.h"
#include "lldb/Host/Socket.h"
#include "lldb/Protocol/MCP/Client.h"
#include "lldb/Protocol/MCP/MCPError.h"
#include "lldb/Protocol/MCP/Protocol.h"
#include "lldb/Protocol/MCP/Resource.h"
#include "lldb/Protocol/MCP/Server.h"
#include "lldb/Protocol/MCP/Tool.h"
#include "lldb/Protocol/MCP/Transport.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Testing/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <chrono>
#include <memory>
#include <optional>

using namespace llvm;
using namespace lldb;
using namespace lldb_private;
using namespace lldb_protocol::mcp;
using namespace lldb_mcp;

#ifndef _WIN32

namespace {

/// A tool that echoes a label and the debugger argument it received, so tests
/// can verify both routing and request-path URI rewriting. It stands in for the
/// backend's `trace_program`, ignoring the plan it is given: what the
/// multiplexer decides about a call is which backend gets it and what the
/// `debugger` argument says by the time it arrives.
class FakeObserveTool : public Tool {
public:
  explicit FakeObserveTool(std::string label)
      : Tool("trace_program", "fake routed tool"), m_label(std::move(label)) {}

  llvm::Expected<CallToolResult> Call(const ToolArguments &args) override {
    std::string debugger;
    if (std::holds_alternative<json::Value>(args))
      if (const json::Object *object =
              std::get<json::Value>(args).getAsObject())
        if (std::optional<StringRef> value = object->getString("debugger"))
          debugger = value->str();

    CallToolResult result;
    result.content.emplace_back(
        TextContent{{m_label + " debugger=" + debugger}});
    return result;
  }

private:
  std::string m_label;
};

/// A tool that always fails, to exercise the routed-call error path.
class FakeFailingTool : public Tool {
public:
  explicit FakeFailingTool(std::string name)
      : Tool(std::move(name), "always fails") {}

  llvm::Expected<CallToolResult> Call(const ToolArguments &) override {
    return llvm::createStringError("backend failure");
  }
};

class FakeResourceProvider : public ResourceProvider {
public:
  using ResourceProvider::ResourceProvider;

  std::vector<Resource> GetResources() const override {
    Resource resource;
    resource.uri = "lldb://debugger/1";
    resource.name = "debugger_1";
    return {resource};
  }

  llvm::Expected<ReadResourceResult>
  ReadResource(llvm::StringRef uri) const override {
    if (uri != "lldb://debugger/1")
      return llvm::make_error<UnsupportedURI>(uri.str());
    TextResourceContents contents;
    contents.uri = "lldb://debugger/1";
    contents.text = "resource contents";
    ReadResourceResult result;
    result.contents.push_back(contents);
    return result;
  }
};

class MultiplexerTest : public testing::Test {
public:
  /// A peer handler that receives forwarded requests but never replies, so
  /// they stay pending on the backend.
  using SilentHandler = MockMessageHandler<ProtocolDescriptor>;

  SubsystemRAII<FileSystem, HostInfo, Socket> subsystems;

  MainLoop loop;
  std::unique_ptr<Multiplexer> mux;
  std::unique_ptr<Client> client;
  std::vector<std::unique_ptr<Server>> servers;
  std::vector<std::unique_ptr<TestTransport<ProtocolDescriptor>>> silent_peers;
  std::vector<std::unique_ptr<SilentHandler>> silent_handlers;

  void SetUp() override {
    auto pair = TestTransport<ProtocolDescriptor>::createConnectedPair(loop);
    mux = std::make_unique<Multiplexer>(std::move(pair.second));
    client = std::make_unique<Client>(std::move(pair.first));
    EXPECT_THAT_ERROR(client->Run(), Succeeded());
  }

  /// Adds a fake remote backend whose routed tool is labelled \p label.
  void AddBackend(lldb::pid_t pid, std::string label) {
    mux->AddBackend(pid, MakeBackend(std::move(label)));
  }

  /// Adds a fake local (in-process) backend, the one that hosts the session a
  /// call naming no debugger lands in.
  void AddLocalBackend(lldb::pid_t pid, std::string label) {
    mux->AddLocalBackend(pid, MakeBackend(std::move(label)));
  }

  /// Builds a running fake backend client wired to a fake server.
  std::unique_ptr<Client> MakeBackend(std::string label) {
    auto pair = TestTransport<ProtocolDescriptor>::createConnectedPair(loop);

    auto server = std::make_unique<Server>("fake", "0.1.0");
    server->AddTool(std::make_unique<FakeObserveTool>(std::move(label)));
    server->AddResourceProvider(std::make_unique<FakeResourceProvider>());
    EXPECT_THAT_ERROR(server->Accept(std::move(pair.second)), Succeeded());

    auto backend = std::make_unique<Client>(std::move(pair.first));
    EXPECT_THAT_ERROR(backend->Run(), Succeeded());
    servers.push_back(std::move(server));
    return backend;
  }

  /// Adds a local backend whose routed tool always errors, to check that a
  /// backend failure reaches the client instead of being swallowed.
  void AddLocalBackendWithFailingObserve(lldb::pid_t pid) {
    auto pair = TestTransport<ProtocolDescriptor>::createConnectedPair(loop);

    auto server = std::make_unique<Server>("fake", "0.1.0");
    server->AddTool(std::make_unique<FakeFailingTool>("trace_program"));
    EXPECT_THAT_ERROR(server->Accept(std::move(pair.second)), Succeeded());

    auto backend = std::make_unique<Client>(std::move(pair.first));
    EXPECT_THAT_ERROR(backend->Run(), Succeeded());
    mux->AddLocalBackend(pid, std::move(backend));
    servers.push_back(std::move(server));
  }

  /// Adds a backend whose peer receives forwarded requests but never replies,
  /// so a forwarded request is left pending. Returns the backend's transport so
  /// a test can simulate a disconnect; the peer and its handler are kept alive
  /// by the fixture.
  TestTransport<ProtocolDescriptor> *AddSilentBackend(lldb::pid_t pid) {
    auto pair = TestTransport<ProtocolDescriptor>::createConnectedPair(loop);
    auto handler = std::make_unique<SilentHandler>();
    // The peer receives the forwarded request but never replies.
    EXPECT_CALL(*handler,
                Received(testing::A<const ProtocolDescriptor::Req &>()))
        .Times(testing::AnyNumber());
    EXPECT_THAT_ERROR(pair.second->RegisterMessageHandler(*handler),
                      Succeeded());
    TestTransport<ProtocolDescriptor> *transport = pair.first.get();
    auto backend = std::make_unique<Client>(std::move(pair.first));
    EXPECT_THAT_ERROR(backend->Run(), Succeeded());
    mux->AddBackend(pid, std::move(backend));
    silent_peers.push_back(std::move(pair.second));
    silent_handlers.push_back(std::move(handler));
    return transport;
  }

  /// Registers the multiplexer's handlers. Backends must be added first.
  void Start() { EXPECT_THAT_ERROR(mux->Run(), Succeeded()); }

  /// Issues a request and runs the loop until its reply arrives.
  template <typename Result>
  llvm::Expected<Result>
  Capture(llvm::unique_function<void(Client::Reply<Result>)> invoke) {
    std::optional<llvm::Expected<Result>> captured;
    invoke([&](llvm::Expected<Result> result) {
      captured = std::move(result);
      loop.RequestTermination();
    });
    bool registered = loop.AddCallback(
        [](MainLoopBase &loop) {
          loop.RequestTermination();
          FAIL() << "timed out waiting for reply";
        },
        std::chrono::seconds(5));
    EXPECT_TRUE(registered);
    EXPECT_THAT_ERROR(loop.Run().takeError(), Succeeded());
    if (!captured)
      return createStringError("no reply");
    return std::move(*captured);
  }

  /// Calls `trace_program` with a plan and, when \p debugger is non-empty, that
  /// debugger argument.
  llvm::Expected<CallToolResult> CallObserve(std::string debugger = "") {
    return Capture<CallToolResult>([&](Client::Reply<CallToolResult> reply) {
      json::Object args{{"plan", json::Object{{"program", "/bin/true"}}}};
      if (!debugger.empty())
        args["debugger"] = std::move(debugger);
      client->ToolsCall(
          CallToolParams{"trace_program", json::Value(std::move(args))},
          std::move(reply));
    });
  }
};

std::string resultText(const CallToolResult &result) {
  std::string text;
  for (const TextContent &content : result.content)
    text += content.text;
  return text;
}

} // namespace

TEST(MultiplexerURITest, ParseGlobalURI) {
  std::optional<RoutedURI> routed =
      ParseGlobalURI("lldb-mcp://instance/42/debugger/3");
  ASSERT_TRUE(routed.has_value());
  EXPECT_EQ(routed->pid, 42u);
  EXPECT_EQ(routed->local, "lldb-mcp://debugger/3");

  routed = ParseGlobalURI("lldb://instance/7/debugger/1/target/0");
  ASSERT_TRUE(routed.has_value());
  EXPECT_EQ(routed->pid, 7u);
  EXPECT_EQ(routed->local, "lldb://debugger/1/target/0");

  // Not instance-qualified.
  EXPECT_FALSE(ParseGlobalURI("lldb-mcp://debugger/1").has_value());
  EXPECT_FALSE(ParseGlobalURI("not a uri").has_value());
  // Non-numeric instance.
  EXPECT_FALSE(ParseGlobalURI("lldb-mcp://instance/x/debugger/1").has_value());
}

TEST(MultiplexerURITest, RewriteURIsToGlobal) {
  EXPECT_EQ(RewriteURIsToGlobal("- lldb-mcp://debugger/1\n", 42),
            "- lldb-mcp://instance/42/debugger/1\n");
  EXPECT_EQ(RewriteURIsToGlobal("lldb://debugger/1/target/0", 7),
            "lldb://instance/7/debugger/1/target/0");
  // Text without a known prefix is unchanged.
  EXPECT_EQ(RewriteURIsToGlobal("no uris here", 1), "no uris here");
}

TEST_F(MultiplexerTest, RunWithoutBackendsFails) {
  EXPECT_THAT_ERROR(mux->Run(), Failed());
}

TEST_F(MultiplexerTest, Initialize) {
  AddBackend(100, "A");
  Start();
  llvm::Expected<InitializeResult> result =
      Capture<InitializeResult>([&](Client::Reply<InitializeResult> reply) {
        client->Initialize(InitializeParams{"2024-11-05", {}, {"test", "0.1"}},
                           std::move(reply));
      });
  ASSERT_THAT_EXPECTED(result, Succeeded());
  EXPECT_EQ(result->serverInfo.name, "lldb-mcp");
}

TEST_F(MultiplexerTest, ToolsListIsOneTool) {
  AddBackend(100, "A");
  Start();
  llvm::Expected<ListToolsResult> result =
      Capture<ListToolsResult>([&](Client::Reply<ListToolsResult> reply) {
        client->ToolsList(std::move(reply));
      });
  ASSERT_THAT_EXPECTED(result, Succeeded());
  std::vector<std::string> names;
  for (const ToolDefinition &tool : result->tools)
    names.push_back(tool.name);
  // Exactly one, so a tool added to a backend does not reach the client without
  // the multiplexer deciding to advertise it.
  EXPECT_THAT(names, testing::ElementsAre("trace_program"));
}

TEST_F(MultiplexerTest, AdvertisedToolCarriesDescriptionAndSchema) {
  AddBackend(100, "A");
  Start();
  llvm::Expected<ListToolsResult> result =
      Capture<ListToolsResult>([&](Client::Reply<ListToolsResult> reply) {
        client->ToolsList(std::move(reply));
      });
  ASSERT_THAT_EXPECTED(result, Succeeded());
  ASSERT_THAT(result->tools, testing::SizeIs(1));
  const ToolDefinition &tool = result->tools.front();
  EXPECT_THAT(tool.description, testing::Not(testing::IsEmpty()));
  ASSERT_TRUE(tool.inputSchema.has_value());
  ASSERT_NE(tool.inputSchema->getAsObject(), nullptr);
  const json::Object &schema = *tool.inputSchema->getAsObject();
  // `plan` is required and `debugger` is not, which is what makes a call with
  // no session named the ordinary way to use the tool.
  const json::Array *required = schema.getArray("required");
  ASSERT_NE(required, nullptr);
  EXPECT_THAT(*required, testing::ElementsAre(json::Value("plan")));
  const json::Object *properties = schema.getObject("properties");
  ASSERT_NE(properties, nullptr);
  EXPECT_NE(properties->get("debugger"), nullptr);
}

TEST_F(MultiplexerTest, UnknownToolErrors) {
  AddLocalBackend(500, "local");
  Start();
  // A name the multiplexer does not advertise is refused here rather than
  // forwarded, even when a backend implements it.
  for (StringRef name :
       {"command", "sessions_list", "session_create", "session_close", "nope"})
    EXPECT_THAT_EXPECTED(
        Capture<CallToolResult>([&](Client::Reply<CallToolResult> reply) {
          client->ToolsCall(CallToolParams{name.str(), {}}, std::move(reply));
        }),
        Failed())
        << "expected \"" << name << "\" to be refused";
}

TEST_F(MultiplexerTest, ObserveRoutesByInstance) {
  AddBackend(100, "A");
  AddBackend(200, "B");
  Start();

  llvm::Expected<CallToolResult> a =
      CallObserve("lldb-mcp://instance/100/debugger/1");
  ASSERT_THAT_EXPECTED(a, Succeeded());
  // Routed to backend A, and the debugger URI was rewritten to backend-local.
  EXPECT_EQ(resultText(*a), "A debugger=lldb-mcp://debugger/1");

  llvm::Expected<CallToolResult> b =
      CallObserve("lldb-mcp://instance/200/debugger/1");
  ASSERT_THAT_EXPECTED(b, Succeeded());
  EXPECT_EQ(resultText(*b), "B debugger=lldb-mcp://debugger/1");
}

TEST_F(MultiplexerTest, ObserveWithoutDebuggerUsesLocalSession) {
  AddLocalBackend(500, "local");
  AddBackend(100, "A");
  Start();

  // With no session named, the call goes to the local backend with no debugger
  // argument at all, which is what lets that backend open a session for it.
  // This is the only route a client has to a session, so it has to work with
  // nothing set up first.
  llvm::Expected<CallToolResult> result = CallObserve();
  ASSERT_THAT_EXPECTED(result, Succeeded());
  EXPECT_EQ(resultText(*result), "local debugger=");
}

TEST_F(MultiplexerTest, ObserveWithoutDebuggerRequiresLocalBackend) {
  AddBackend(100, "A"); // Remote only, no local backend.
  Start();
  EXPECT_THAT_EXPECTED(CallObserve(), Failed());
}

TEST_F(MultiplexerTest, ObserveUnknownInstanceErrors) {
  AddBackend(100, "A");
  Start();
  EXPECT_THAT_EXPECTED(CallObserve("lldb-mcp://instance/999/debugger/1"),
                       Failed());
}

TEST_F(MultiplexerTest, ObserveMalformedDebuggerURIErrors) {
  AddLocalBackend(500, "local");
  Start();
  // Not instance-qualified, so ParseGlobalURI rejects it. Refused rather than
  // quietly treated as "no debugger named", since the caller did name one.
  EXPECT_THAT_EXPECTED(CallObserve("lldb-mcp://debugger/1"), Failed());
}

TEST_F(MultiplexerTest, ObserveSurfacesBackendError) {
  AddLocalBackendWithFailingObserve(500);
  Start();
  EXPECT_THAT_EXPECTED(CallObserve(), Failed());
}

TEST_F(MultiplexerTest, ResourcesListAggregatesAndRewrites) {
  AddBackend(100, "A");
  AddBackend(200, "B");
  Start();
  llvm::Expected<ListResourcesResult> result = Capture<ListResourcesResult>(
      [&](Client::Reply<ListResourcesResult> reply) {
        client->ResourcesList(std::move(reply));
      });
  ASSERT_THAT_EXPECTED(result, Succeeded());
  std::vector<std::string> uris;
  for (const Resource &resource : result->resources)
    uris.push_back(resource.uri);
  EXPECT_THAT(uris,
              testing::UnorderedElementsAre("lldb://instance/100/debugger/1",
                                            "lldb://instance/200/debugger/1"));
}

TEST_F(MultiplexerTest, ResourcesReadRoutesAndRewrites) {
  AddBackend(100, "A");
  AddBackend(200, "B");
  Start();
  llvm::Expected<ReadResourceResult> result =
      Capture<ReadResourceResult>([&](Client::Reply<ReadResourceResult> reply) {
        client->ResourcesRead(
            ReadResourceParams{"lldb://instance/200/debugger/1"},
            std::move(reply));
      });
  ASSERT_THAT_EXPECTED(result, Succeeded());
  ASSERT_THAT(result->contents, testing::SizeIs(1));
  // The response URI is rewritten back to the global form.
  EXPECT_EQ(result->contents.front().uri, "lldb://instance/200/debugger/1");
  EXPECT_EQ(result->contents.front().text, "resource contents");
}

TEST_F(MultiplexerTest, ResourcesReadMalformedURIErrors) {
  AddBackend(100, "A");
  Start();
  llvm::Expected<ReadResourceResult> result =
      Capture<ReadResourceResult>([&](Client::Reply<ReadResourceResult> reply) {
        client->ResourcesRead(ReadResourceParams{"lldb://debugger/1"},
                              std::move(reply));
      });
  EXPECT_THAT_EXPECTED(result, Failed());
}

TEST_F(MultiplexerTest, ResourcesReadUnknownInstanceErrors) {
  AddBackend(100, "A");
  Start();
  llvm::Expected<ReadResourceResult> result =
      Capture<ReadResourceResult>([&](Client::Reply<ReadResourceResult> reply) {
        client->ResourcesRead(
            ReadResourceParams{"lldb://instance/999/debugger/1"},
            std::move(reply));
      });
  EXPECT_THAT_EXPECTED(result, Failed());
}

TEST_F(MultiplexerTest, ShutdownFailsPendingBackendRequest) {
  AddSilentBackend(100);
  Start();

  std::optional<llvm::Expected<CallToolResult>> captured;
  json::Object args{{"plan", json::Object{{"program", "/bin/true"}}},
                    {"debugger", "lldb-mcp://instance/100/debugger/1"}};
  client->ToolsCall(
      CallToolParams{"trace_program", json::Value(std::move(args))},
      [&](llvm::Expected<CallToolResult> result) {
        captured = std::move(result);
        loop.RequestTermination();
      });

  // The request is forwarded to the silent backend and left pending; shutting
  // down must fail it so the client gets an error instead of hanging. The
  // delay lets the forward complete first (sends run as undelayed callbacks).
  bool scheduled = loop.AddCallback([&](MainLoopBase &) { mux->Shutdown(); },
                                    std::chrono::milliseconds(100));
  EXPECT_TRUE(scheduled);
  loop.AddCallback(
      [](MainLoopBase &l) {
        l.RequestTermination();
        FAIL() << "timed out waiting for reply";
      },
      std::chrono::seconds(5));

  EXPECT_THAT_ERROR(loop.Run().takeError(), Succeeded());
  ASSERT_TRUE(captured.has_value());
  EXPECT_THAT_EXPECTED(*captured, Failed());
}

TEST_F(MultiplexerTest, BackendDisconnectFailsPendingRequest) {
  TestTransport<ProtocolDescriptor> *backend = AddSilentBackend(100);
  Start();

  std::optional<llvm::Expected<CallToolResult>> captured;
  json::Object args{{"plan", json::Object{{"program", "/bin/true"}}},
                    {"debugger", "lldb-mcp://instance/100/debugger/1"}};
  client->ToolsCall(
      CallToolParams{"trace_program", json::Value(std::move(args))},
      [&](llvm::Expected<CallToolResult> result) {
        captured = std::move(result);
        loop.RequestTermination();
      });

  // Once the request is pending on the backend, a backend disconnect must fail
  // it rather than leave the client hanging.
  bool scheduled =
      loop.AddCallback([&](MainLoopBase &) { backend->SimulateClosed(); },
                       std::chrono::milliseconds(100));
  EXPECT_TRUE(scheduled);
  loop.AddCallback(
      [](MainLoopBase &l) {
        l.RequestTermination();
        FAIL() << "timed out waiting for reply";
      },
      std::chrono::seconds(5));

  EXPECT_THAT_ERROR(loop.Run().takeError(), Succeeded());
  ASSERT_TRUE(captured.has_value());
  EXPECT_THAT_EXPECTED(*captured, Failed());
}

TEST_F(MultiplexerTest, ResourcesListSurvivesBackendDisconnect) {
  // A backend that never answers must not hang a fanned-out aggregation once it
  // disconnects. The healthy backend's result still comes back, and the one
  // that failed is omitted rather than failing the whole listing.
  TestTransport<ProtocolDescriptor> *silent = AddSilentBackend(200);
  AddBackend(100, "A");
  Start();

  std::optional<llvm::Expected<ListResourcesResult>> captured;
  client->ResourcesList([&](llvm::Expected<ListResourcesResult> result) {
    captured = std::move(result);
    loop.RequestTermination();
  });
  // Disconnect the silent backend after the fan-out has been issued.
  EXPECT_TRUE(loop.AddPendingCallback(
      [silent](MainLoopBase &) { silent->SimulateClosed(); }));
  EXPECT_TRUE(loop.AddCallback(
      [](MainLoopBase &loop) {
        loop.RequestTermination();
        FAIL() << "resources/list hung after a backend disconnected";
      },
      std::chrono::seconds(5)));
  EXPECT_THAT_ERROR(loop.Run().takeError(), Succeeded());

  ASSERT_TRUE(captured.has_value());
  ASSERT_THAT_EXPECTED(*captured, Succeeded());
  std::vector<std::string> uris;
  for (const Resource &resource : (*captured)->resources)
    uris.push_back(resource.uri);
  EXPECT_THAT(uris,
              testing::ElementsAre("lldb://instance/100/debugger/1"));
}

#endif // ifndef _WIN32
