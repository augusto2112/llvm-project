//===- SerializeValue.h ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_PROTOCOL_MCP_SERIALIZEVALUE_H
#define LLDB_SOURCE_PLUGINS_PROTOCOL_MCP_SERIALIZEVALUE_H

#include "ValueNode.h"
#include "llvm/Support/JSON.h"
#include <string>

namespace lldb_private::mcp {

struct SerializeValueOptions {
  /// How many levels of children to expand.
  unsigned MaxDepth = 2;

  /// Total nodes to emit. Depth alone is not a bound on a value graph, where a
  /// handful of hops can reach an arbitrarily large object, so the node cap is
  /// what actually stops the walk.
  unsigned MaxNodes = 64;

  /// Children to emit per node.
  unsigned MaxChildren = 16;

  unsigned MaxStringLength = 128;

  /// When set, elision markers point here instead of dead-ending, so going
  /// deeper never requires re-running the program.
  std::string ArtifactRef;
};

/// Renders the tree rooted at \p Root as JSON within the budgets in \p Opts.
/// Anything the budgets cut is replaced by an elision marker rather than
/// dropped silently.
llvm::json::Value SerializeValue(ValueNode &Root,
                                 const SerializeValueOptions &Opts);

} // namespace lldb_private::mcp

#endif
