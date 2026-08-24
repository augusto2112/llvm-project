//===- ValueObjectNode.cpp ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ValueObjectNode.h"
#include "SerializeValue.h"
#include "ValueNode.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/Utility/Status.h"
#include "lldb/ValueObject/ValueObject.h"
#include "llvm/ADT/Hashing.h"
#include "lldb/lldb-defines.h"
#include "lldb/lldb-forward.h"
#include "lldb/lldb-types.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace lldb_private;
using namespace lldb_private::mcp;

ValueObjectNode::ValueObjectNode(lldb::ValueObjectSP ValueSP)
    : m_value(std::move(ValueSP)) {}

llvm::StringRef ValueObjectNode::GetName() {
  if (!m_value)
    return llvm::StringRef();
  // ConstString contents live in a pool for the life of the process, so a
  // reference into one outlives this node without being copied.
  return m_value->GetName().GetStringRef();
}

llvm::StringRef ValueObjectNode::GetTypeName() {
  if (!m_value)
    return llvm::StringRef();
  return m_value->GetTypeName().GetStringRef();
}

std::optional<std::string> ValueObjectNode::GetSummary() {
  if (!m_value)
    return std::nullopt;
  const char *Summary = m_value->GetSummaryAsCString();
  if (!Summary || !*Summary)
    return std::nullopt;
  return std::string(Summary);
}

std::optional<std::string> ValueObjectNode::GetValueString() {
  if (!m_value)
    return std::nullopt;
  const char *ValueStr = m_value->GetValueAsCString();
  if (!ValueStr || !*ValueStr)
    return std::nullopt;
  return std::string(ValueStr);
}

Availability ValueObjectNode::GetAvailability() {
  if (!m_value)
    return Availability::Error;

  const Status &Err = m_value->GetError();
  if (Err.Success())
    return Availability::Available;

  // ValueObject reports every one of these as a plain error, so the kind is
  // only recoverable from the message. Optimization is tested first because a
  // gap in a location list is described as both a missing location and an
  // optimized-out value, and the former is the less useful of the two.
  //
  // A hint is excluded from the match. The expression evaluator's hint for a
  // call into a symbol the target does not hold reads "perhaps because it was
  // optimized out by the compiler", which classified a missing symbol as an
  // optimized-out value and sent a reader to rebuild a program that was already
  // unoptimized. Speculation about a cause must not outrank the failure itself.
  llvm::StringRef Msg = Err.AsCString("");
  Msg = Msg.substr(0, Msg.find("Hint:"));
  if (Msg.contains("optimized out"))
    return Availability::OptimizedOut;
  if (Msg.contains("no location") || Msg.contains("not available"))
    return Availability::NoLocation;
  if (Msg.contains("no debug info") || Msg.contains("incomplete type"))
    return Availability::NoDebugInfo;
  return Availability::Error;
}

std::string ValueObjectNode::GetUnavailableReason() {
  if (!m_value)
    return {};

  const Status &Err = m_value->GetError();
  if (Err.Success())
    return {};
  return CondenseDiagnostic(Err.AsCString(""), MaxReasonLength);
}

uint64_t ValueObjectNode::GetIdentity() {
  if (!m_value)
    return 0;
  lldb::addr_t Addr = m_value->GetLoadAddress();
  if (Addr == LLDB_INVALID_ADDRESS)
    return 0;

  // An address alone does not identify an object: a struct, its first member,
  // and that member's first member all begin at the same address, so an
  // address-only identity reports a nested aggregate as a cycle and loses the
  // value it was standing in for. The type is what separates them.
  return llvm::hash_combine(Addr, m_value->GetTypeName().GetStringRef());
}

size_t ValueObjectNode::GetNumChildren() {
  if (!m_value)
    return 0;
  llvm::Expected<uint32_t> Count = m_value->GetNumChildren();
  if (!Count) {
    // A node whose child count cannot be determined is rendered as a leaf
    // rather than failing the whole tree.
    llvm::consumeError(Count.takeError());
    return 0;
  }
  return *Count;
}

std::unique_ptr<ValueNode> ValueObjectNode::GetChildAtIndex(size_t Idx) {
  if (!m_value)
    return nullptr;
  lldb::ValueObjectSP Child =
      m_value->GetChildAtIndex(static_cast<uint32_t>(Idx));
  if (!Child)
    return nullptr;
  return std::make_unique<ValueObjectNode>(std::move(Child));
}
