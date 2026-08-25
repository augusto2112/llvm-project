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

  /// When set, the walk spends this budget instead of MaxNodes and leaves what
  /// it did not spend, so that several values can share one.
  ///
  /// A caller rendering many values into a single response -- every local of a
  /// frame -- needs the response bounded rather than each value in it. Measured
  /// on a compiler stopped inside its instruction selector, a frame of 32 locals
  /// each given its own 64-node budget produced 8.6 kB of pass-manager and
  /// target-machine interior, an order of magnitude more than the backtrace and
  /// source it was reported beside.
  unsigned *SharedBudget = nullptr;

  /// Children to emit per node.
  unsigned MaxChildren = 16;

  unsigned MaxStringLength = 128;

  /// Characters a whole rendered value may take before it is reported as its own
  /// value alone, with the rest left to the artifact.
  ///
  /// Depth and node counts bound the walk; they do not bound the reading. A
  /// capture of one pointer into a compiler's value hierarchy came back as the
  /// fields of the object it pointed at -- `HasDescriptor`, `MetadataIndex`,
  /// `SubclassID`, `UseList` -- which named no instruction and cost a kilobyte
  /// three times over, since a summary repeats a value in its histogram and again
  /// in its transitions. Past this size the address is the identifying part, and
  /// what the object holds is a question for a path that names the field.
  ///
  /// Zero leaves the reading unbounded, which is what the terminal event's locals
  /// want: there the shared node budget is the bound and there is no aggregate to
  /// repeat anything into.
  unsigned MaxRenderedChars = 0;

  /// When set, elision markers point here instead of dead-ending, so going
  /// deeper never requires re-running the program.
  std::string ArtifactRef;

  /// When set, records what the walk met: whether any node had a formatter
  /// summary, and whether any was expanded into its members for want of one.
  ///
  /// The two together are what let a run say something a caller cannot work out
  /// alone. A value expanded into members looks the same whether the type has no
  /// custom rendering or the formatter that would have given it one was never
  /// loaded, and those want opposite responses: read the members, or load the
  /// formatters.
  bool *SawSummary = nullptr;
  bool *SawExpansion = nullptr;
};

/// Reduces a compiler or debugger diagnostic to the one line that says what went
/// wrong, dropping the position prefix, the language note and the caret art that
/// a terminal reader wants and a machine reader pays for.
///
/// A hint is kept and appended, because for the failures worth explaining it is
/// the hint that names the fix.
std::string CondenseDiagnostic(llvm::StringRef Message, unsigned MaxLength);

/// Renders the tree rooted at \p Root as JSON within the budgets in \p Opts.
/// Anything the budgets cut is replaced by an elision marker rather than
/// dropped silently.
llvm::json::Value SerializeValue(ValueNode &Root,
                                 const SerializeValueOptions &Opts);

} // namespace lldb_private::mcp

#endif
