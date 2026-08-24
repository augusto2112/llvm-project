//===- ValueObjectNode.h --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_PROTOCOL_MCP_VALUEOBJECTNODE_H
#define LLDB_SOURCE_PLUGINS_PROTOCOL_MCP_VALUEOBJECTNODE_H

#include "ValueNode.h"
#include "lldb/lldb-forward.h"
#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace lldb_private::mcp {

/// Adapts a ValueObject to the ValueNode interface. A null value behaves as an
/// empty node in the Error state.
class ValueObjectNode : public ValueNode {
public:
  explicit ValueObjectNode(lldb::ValueObjectSP ValueSP);

  llvm::StringRef GetName() override;
  llvm::StringRef GetTypeName() override;
  std::optional<std::string> GetSummary() override;
  std::optional<std::string> GetValueString() override;
  Availability GetAvailability() override;
  std::string GetUnavailableReason() override;
  uint64_t GetIdentity() override;
  size_t GetNumChildren() override;
  std::unique_ptr<ValueNode> GetChildAtIndex(size_t Idx) override;

  /// A diagnostic long enough to name the symbol or the identifier it is about,
  /// and short enough that a capture failing at every hit of a hot tracepoint
  /// cannot dominate the response it appears in.
  static constexpr unsigned MaxReasonLength = 200;

private:
  lldb::ValueObjectSP m_value;
};

} // namespace lldb_private::mcp

#endif
