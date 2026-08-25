//===-- EntryTrampoline.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_TARGET_ENTRYTRAMPOLINE_H
#define LLDB_TARGET_ENTRYTRAMPOLINE_H

#include "lldb/lldb-types.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace lldb_private {

/// Bytes an entry trampoline occupies. The two instructions load the target
/// from the eight bytes that follow them, so the literal is part of the
/// patched region rather than stored elsewhere: a trampoline that reached
/// outside itself would need an allocation whose lifetime nothing owns.
constexpr size_t kEntryTrampolineSize = 16;

/// Encodes an arm64 branch to \p Target, to be written over a function's
/// entry.
///
/// Nothing is saved and nothing is restored. At a function's entry the
/// arguments are already in the registers the replacement expects, because it
/// has the same signature, and `x16` is architecturally scratch at a call
/// boundary -- it is reserved for exactly this, a linker's veneer. So the
/// branch is a jump rather than a call, and the replacement returns straight
/// to the original caller.
///
/// The target is loaded from a literal rather than encoded as a displacement,
/// which costs eight bytes and buys the whole address space: JIT'd code is
/// wherever the inferior's allocator put it, which is not reliably within
/// `b`'s 128MB reach.
std::array<uint8_t, kEntryTrampolineSize>
EncodeEntryTrampoline(lldb::addr_t Target);

} // namespace lldb_private

#endif // LLDB_TARGET_ENTRYTRAMPOLINE_H
