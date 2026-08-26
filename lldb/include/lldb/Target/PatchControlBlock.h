//===-- PatchControlBlock.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_TARGET_PATCHCONTROLBLOCK_H
#define LLDB_TARGET_PATCHCONTROLBLOCK_H

#include "llvm/ADT/ArrayRef.h"
#include <cstdint>
#include <vector>

namespace lldb_private {

/// One captured value, as the inferior writes it.
///
/// The value is eight raw bytes rather than a typed union: the capture's type
/// is read from the patched function's debug info, so encoding it here as well
/// would be a second source of truth that could disagree with the first.
struct PatchRecord {
  uint32_t Site = 0;
  uint32_t Capture = 0;
  uint64_t Value = 0;
};

/// The shared block's fixed head, which the record ring follows.
struct PatchRingHeader {
  /// Records the inferior has written, ever. Never reset, so it doubles as the
  /// ring index and as the count the debugger compares against.
  uint64_t Seq = 0;

  /// Records the debugger has read. Written by the debugger only.
  uint64_t Drained = 0;

  /// Records the ring holds. A power of two, so the inferior indexes with a
  /// mask rather than a division.
  uint64_t Capacity = 0;

  /// Unread records at which the inferior traps to ask for a drain.
  uint64_t HighWater = 0;
};

/// One site's own state, at its own address.
///
/// Deliberately not an array in the header. An array index's offset depends on
/// the array's length, so growing the site count would invalidate the offsets
/// already compiled into every patch running in the program, making the count a
/// hard limit fixed when the block was allocated.
struct PatchSiteSlot {
  uint64_t Hits = 0;
  uint64_t CondTrue = 0;
  /// Whether the site does anything at all. The debugger opens and closes it to
  /// implement gating that depends on the debugger's own state.
  uint8_t Gate = 0;
};

/// What one drain produced.
struct PatchDrain {
  /// New records, oldest first.
  std::vector<PatchRecord> Records;

  /// Records that did not reach the caller: those the ring overwrote before
  /// they were read, and those that fell outside a truncated ring buffer.
  /// Reported rather than absorbed: a short list that looks complete is worse
  /// than one that says what is missing.
  uint64_t Lost = 0;

  /// What the debugger should store back as \ref PatchRingHeader::Drained.
  uint64_t NewDrained = 0;
};

constexpr size_t kPatchRecordSize = 16;
constexpr size_t kPatchRingHeaderSize = 32;
constexpr size_t kPatchSiteSlotSize = 24;

/// Records the ring holds by default: 64KB, which at three quarters full costs
/// one stop per three thousand captured values instead of one per value.
constexpr uint64_t kDefaultRingCapacity = 4096;

PatchRecord DecodePatchRecord(llvm::ArrayRef<uint8_t> Bytes);
void EncodePatchRecord(const PatchRecord &Rec,
                       llvm::MutableArrayRef<uint8_t> Out);

/// Reads every record written since \p Header.Drained out of \p RingBytes.
///
/// Tolerates a header that cannot be true -- a drained count ahead of the
/// sequence, a capacity of zero, a ring shorter than the capacity claims --
/// because the header was read from a live process and a torn read must not
/// become an out-of-bounds one.
PatchDrain DrainPatchRing(const PatchRingHeader &Header,
                          llvm::ArrayRef<uint8_t> RingBytes);

} // namespace lldb_private

#endif // LLDB_TARGET_PATCHCONTROLBLOCK_H
