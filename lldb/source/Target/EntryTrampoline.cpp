//===-- EntryTrampoline.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/EntryTrampoline.h"
#include "llvm/Support/Endian.h"

using namespace lldb_private;

namespace {
/// `ldr x16, .+8`: LDR (literal), 64-bit, imm19 of 2 (two instructions ahead),
/// Rt of 16.
constexpr uint32_t kLdrX16Literal = 0x58000050;

/// `br x16`.
constexpr uint32_t kBrX16 = 0xD61F0200;
} // namespace

std::array<uint8_t, kEntryTrampolineSize>
lldb_private::EncodeEntryTrampoline(lldb::addr_t Target) {
  std::array<uint8_t, kEntryTrampolineSize> Bytes = {};
  llvm::support::endian::write32le(Bytes.data(), kLdrX16Literal);
  llvm::support::endian::write32le(Bytes.data() + 4, kBrX16);
  llvm::support::endian::write64le(Bytes.data() + kEntryTrampolineTargetOffset,
                                   Target);
  return Bytes;
}
