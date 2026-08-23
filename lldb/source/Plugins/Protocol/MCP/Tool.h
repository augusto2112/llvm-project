//===- Tool.h -------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_PROTOCOL_MCP_TOOL_H
#define LLDB_SOURCE_PLUGINS_PROTOCOL_MCP_TOOL_H

#include "lldb/Protocol/MCP/Protocol.h"
#include "lldb/Protocol/MCP/Tool.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include <optional>

namespace lldb_private::mcp {

class CommandTool : public lldb_protocol::mcp::Tool {
public:
  using lldb_protocol::mcp::Tool::Tool;
  ~CommandTool() = default;

  llvm::Expected<lldb_protocol::mcp::CallToolResult>
  Call(const lldb_protocol::mcp::ToolArguments &args) override;

  std::optional<llvm::json::Value> GetSchema() const override;
};

/// Runs an observation plan: a program under a set of tracepoints, reported as
/// one document rather than as a conversation.
class ObserveTool : public lldb_protocol::mcp::Tool {
public:
  using lldb_protocol::mcp::Tool::Tool;
  ~ObserveTool() = default;

  llvm::Expected<lldb_protocol::mcp::CallToolResult>
  Call(const lldb_protocol::mcp::ToolArguments &args) override;

  std::optional<llvm::json::Value> GetSchema() const override;
};

/// What the `observe` tool is for, in the terms a caller cannot infer from the
/// schema. Out of line because it is far too long to read at the registration
/// site, where it would bury the list of tools.
extern const llvm::StringLiteral ObserveToolDescription;

class DebuggerListTool : public lldb_protocol::mcp::Tool {
public:
  using lldb_protocol::mcp::Tool::Tool;
  ~DebuggerListTool() = default;

  llvm::Expected<lldb_protocol::mcp::CallToolResult>
  Call(const lldb_protocol::mcp::ToolArguments &args) override;
};

class DebuggerCreateTool : public lldb_protocol::mcp::Tool {
public:
  using lldb_protocol::mcp::Tool::Tool;
  ~DebuggerCreateTool() = default;

  llvm::Expected<lldb_protocol::mcp::CallToolResult>
  Call(const lldb_protocol::mcp::ToolArguments &args) override;
};

class DebuggerDeleteTool : public lldb_protocol::mcp::Tool {
public:
  using lldb_protocol::mcp::Tool::Tool;
  ~DebuggerDeleteTool() = default;

  llvm::Expected<lldb_protocol::mcp::CallToolResult>
  Call(const lldb_protocol::mcp::ToolArguments &args) override;

  std::optional<llvm::json::Value> GetSchema() const override;
};

} // namespace lldb_private::mcp

#endif
