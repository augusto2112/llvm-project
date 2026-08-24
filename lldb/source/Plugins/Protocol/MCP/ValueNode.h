//===- ValueNode.h --------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_PROTOCOL_MCP_VALUENODE_H
#define LLDB_SOURCE_PLUGINS_PROTOCOL_MCP_VALUENODE_H

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace lldb_private::mcp {

/// Why a value could not be read. The kinds are kept apart because a caller
/// must be able to tell "no location at this pc" from "no debug info at all";
/// conflating them makes an optimized-out variable indistinguishable from a
/// null one.
enum class Availability {
  Available,
  OptimizedOut,
  NoLocation,
  NoDebugInfo,
  Error,
};

/// A node in a value tree, abstracted away from ValueObject so serialization
/// can be exercised without a live process.
class ValueNode {
public:
  virtual ~ValueNode() = default;

  /// The returned references must outlive the node.
  virtual llvm::StringRef GetName() = 0;
  virtual llvm::StringRef GetTypeName() = 0;

  /// A formatter-provided one-line rendering, when one exists. Preferred over
  /// expanding children: it is both denser and cheaper.
  virtual std::optional<std::string> GetSummary() = 0;

  virtual std::optional<std::string> GetValueString() = 0;
  virtual Availability GetAvailability() = 0;

  /// Why this value could not be read, in one line, or empty when there is
  /// nothing to say beyond the kind.
  ///
  /// The kind alone is a classification of a message, and a classification of a
  /// message that is thrown away cannot be checked: measured on a capture
  /// calling a function whose symbol the binary did not contain, the error read
  /// "Couldn't look up symbols ... perhaps because it was optimized out by the
  /// compiler", which classified as `optimized_out` and sent the reader to
  /// rebuild without optimization for a program that was already unoptimized.
  /// The message names the fix; the kind only groups it.
  virtual std::string GetUnavailableReason() = 0;

  /// A stable identity for cycle detection, or 0 when the node has none.
  ///
  /// Two nodes sharing an identity are taken to be the same object, and the
  /// second is reported rather than expanded. A load address alone is therefore
  /// not enough: a struct, its first member, and that member's first member all
  /// begin at the same address, and treating them as one loses the innermost
  /// value. An implementation must distinguish nodes that merely start at the
  /// same place, for instance by combining the address with the type. Two
  /// nodes sharing an identity denote the same underlying object.
  virtual uint64_t GetIdentity() = 0;

  virtual size_t GetNumChildren() = 0;

  /// Returns null when the child cannot be produced.
  virtual std::unique_ptr<ValueNode> GetChildAtIndex(size_t Idx) = 0;
};

} // namespace lldb_private::mcp

#endif
