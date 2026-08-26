//===-- PatchControlBlock.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/PatchControlBlock.h"
#include "llvm/Support/Endian.h"
#include <algorithm>

using namespace lldb_private;

PatchRecord lldb_private::DecodePatchRecord(llvm::ArrayRef<uint8_t> Bytes) {
  PatchRecord Rec;
  if (Bytes.size() < kPatchRecordSize)
    return Rec;
  Rec.Site = llvm::support::endian::read32le(Bytes.data());
  Rec.Capture = llvm::support::endian::read32le(Bytes.data() + 4);
  Rec.Value = llvm::support::endian::read64le(Bytes.data() + 8);
  return Rec;
}

void lldb_private::EncodePatchRecord(const PatchRecord &Rec,
                                    llvm::MutableArrayRef<uint8_t> Out) {
  if (Out.size() < kPatchRecordSize)
    return;
  llvm::support::endian::write32le(Out.data(), Rec.Site);
  llvm::support::endian::write32le(Out.data() + 4, Rec.Capture);
  llvm::support::endian::write64le(Out.data() + 8, Rec.Value);
}

PatchDrain lldb_private::DrainPatchRing(const PatchRingHeader &Header,
                                       llvm::ArrayRef<uint8_t> RingBytes) {
  PatchDrain Drain;
  Drain.NewDrained = Header.Drained;

  if (Header.Capacity == 0 || Header.Seq <= Header.Drained)
    return Drain;

  Drain.NewDrained = Header.Seq;

  // The ring keeps the most recent Capacity records, so anything older than
  // that is gone no matter what the debugger last read.
  const uint64_t Unread = Header.Seq - Header.Drained;
  const uint64_t Readable = std::min(Unread, Header.Capacity);
  Drain.Lost = Unread - Readable;

  const uint64_t Available = RingBytes.size() / kPatchRecordSize;
  const uint64_t First = Header.Seq - Readable;

  Drain.Records.reserve(Readable);
  for (uint64_t Seq = First; Seq < Header.Seq; ++Seq) {
    const uint64_t Slot = Seq % Header.Capacity;
    // A ring shorter than the capacity claims means the read was truncated.
    // Stopping is right; reading past the buffer is not.
    if (Slot >= Available)
      continue;
    Drain.Records.push_back(DecodePatchRecord(
        RingBytes.slice(Slot * kPatchRecordSize, kPatchRecordSize)));
  }
  return Drain;
}
