//===-- PatchControlBlockTest.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/PatchControlBlock.h"
#include "gtest/gtest.h"
#include <vector>

using namespace lldb_private;

/// A ring holding \p Capacity records, with \p Written records stored at their
/// natural positions, so a test can describe a ring by what the inferior would
/// have put in it.
static std::vector<uint8_t> MakeRing(uint64_t Capacity,
                                     llvm::ArrayRef<PatchRecord> Written) {
  std::vector<uint8_t> Bytes(Capacity * kPatchRecordSize, 0);
  for (uint64_t I = 0; I < Written.size(); ++I) {
    uint64_t Slot = I % Capacity;
    EncodePatchRecord(Written[I],
                      llvm::MutableArrayRef<uint8_t>(
                          Bytes.data() + Slot * kPatchRecordSize,
                          kPatchRecordSize));
  }
  return Bytes;
}

TEST(PatchControlBlockTest, RoundTripsARecord) {
  PatchRecord Rec{7, 3, 4200, 0xDEADBEEFCAFEF00D};
  std::vector<uint8_t> Bytes(kPatchRecordSize, 0);
  EncodePatchRecord(Rec, Bytes);
  PatchRecord Back = DecodePatchRecord(Bytes);
  EXPECT_EQ(Rec.Site, Back.Site);
  EXPECT_EQ(Rec.Capture, Back.Capture);
  EXPECT_EQ(Rec.Hit, Back.Hit);
  EXPECT_EQ(Rec.Value, Back.Value);
}

// The hit a record belongs to is what joins that hit's several captures back
// together, so it has to survive the ring rather than be inferred from the order
// records arrive in -- which two threads inside one site interleave.
TEST(PatchControlBlockTest, KeepsEachRecordWithItsOwnHit) {
  std::vector<PatchRecord> Written{
      {1, 0, 1, 10}, {1, 0, 2, 30}, {1, 1, 1, 20}, {1, 1, 2, 40}};
  auto Ring = MakeRing(8, Written);
  PatchRingHeader Header{4, 0, 8, 6};
  PatchDrain Drain = DrainPatchRing(Header, Ring);
  ASSERT_EQ(4u, Drain.Records.size());
  EXPECT_EQ(1u, Drain.Records[0].Hit);
  EXPECT_EQ(2u, Drain.Records[1].Hit);
  EXPECT_EQ(1u, Drain.Records[2].Hit);
  EXPECT_EQ(2u, Drain.Records[3].Hit);
}

// The sizes are baked into the generated C source as literals, so a change here
// without a matching change there would misalign every record.
TEST(PatchControlBlockTest, HasTheDocumentedSizes) {
  EXPECT_EQ(24u, kPatchRecordSize);
  EXPECT_EQ(32u, kPatchRingHeaderSize);
  EXPECT_EQ(24u, kPatchSiteSlotSize);
}

TEST(PatchControlBlockTest, DrainsNothingFromAnUntouchedRing) {
  PatchRingHeader Header{0, 0, 8, 6};
  auto Ring = MakeRing(8, {});
  PatchDrain Drain = DrainPatchRing(Header, Ring);
  EXPECT_TRUE(Drain.Records.empty());
  EXPECT_EQ(0u, Drain.Lost);
  EXPECT_EQ(0u, Drain.NewDrained);
}

TEST(PatchControlBlockTest, DrainsRecordsInWriteOrder) {
  std::vector<PatchRecord> Written{{1, 0, 1, 10}, {1, 1, 1, 20}, {2, 0, 1, 30}};
  auto Ring = MakeRing(8, Written);
  PatchRingHeader Header{3, 0, 8, 6};
  PatchDrain Drain = DrainPatchRing(Header, Ring);
  ASSERT_EQ(3u, Drain.Records.size());
  EXPECT_EQ(10u, Drain.Records[0].Value);
  EXPECT_EQ(20u, Drain.Records[1].Value);
  EXPECT_EQ(30u, Drain.Records[2].Value);
  EXPECT_EQ(0u, Drain.Lost);
  EXPECT_EQ(3u, Drain.NewDrained);
}

TEST(PatchControlBlockTest, DrainsOnlyWhatIsNew) {
  std::vector<PatchRecord> Written{{1, 0, 1, 10}, {1, 0, 2, 20}, {1, 0, 3, 30}};
  auto Ring = MakeRing(8, Written);
  PatchRingHeader Header{3, 1, 8, 6};
  PatchDrain Drain = DrainPatchRing(Header, Ring);
  ASSERT_EQ(2u, Drain.Records.size());
  EXPECT_EQ(20u, Drain.Records[0].Value);
  EXPECT_EQ(30u, Drain.Records[1].Value);
  EXPECT_EQ(0u, Drain.Lost);
  EXPECT_EQ(3u, Drain.NewDrained);
}

// Once more records have been written than the ring holds, the oldest are gone.
// How many is reported rather than absorbed, because a short list that looks
// complete is worse than a list that says what is missing.
TEST(PatchControlBlockTest, ReportsWhatTheRingOverwrote) {
  std::vector<PatchRecord> Written;
  for (uint64_t I = 0; I < 10; ++I)
    Written.push_back({1, 0, I + 1, I});
  auto Ring = MakeRing(4, Written);
  PatchRingHeader Header{10, 0, 4, 3};
  PatchDrain Drain = DrainPatchRing(Header, Ring);
  EXPECT_EQ(4u, Drain.Records.size());
  EXPECT_EQ(6u, Drain.Lost);
  EXPECT_EQ(10u, Drain.NewDrained);
  // The four survivors are the four most recent, in write order.
  EXPECT_EQ(6u, Drain.Records[0].Value);
  EXPECT_EQ(7u, Drain.Records[1].Value);
  EXPECT_EQ(8u, Drain.Records[2].Value);
  EXPECT_EQ(9u, Drain.Records[3].Value);
}

TEST(PatchControlBlockTest, ReadsAcrossTheWrapPoint) {
  std::vector<PatchRecord> Written;
  for (uint64_t I = 0; I < 6; ++I)
    Written.push_back({1, 0, I + 1, I});
  auto Ring = MakeRing(4, Written);
  PatchRingHeader Header{6, 3, 4, 3};
  PatchDrain Drain = DrainPatchRing(Header, Ring);
  ASSERT_EQ(3u, Drain.Records.size());
  EXPECT_EQ(3u, Drain.Records[0].Value);
  EXPECT_EQ(4u, Drain.Records[1].Value);
  EXPECT_EQ(5u, Drain.Records[2].Value);
  EXPECT_EQ(0u, Drain.Lost);
  EXPECT_EQ(6u, Drain.NewDrained);
}

// A header claiming fewer records drained than written cannot be trusted to
// index the ring, but it must not read out of bounds either.
TEST(PatchControlBlockTest, ToleratesADrainedCountAheadOfSeq) {
  auto Ring = MakeRing(4, {{1, 0, 1, 10}});
  PatchRingHeader Header{1, 5, 4, 3};
  PatchDrain Drain = DrainPatchRing(Header, Ring);
  EXPECT_TRUE(Drain.Records.empty());
  EXPECT_EQ(0u, Drain.Lost);
}

TEST(PatchControlBlockTest, ToleratesAZeroCapacity) {
  PatchRingHeader Header{4, 0, 0, 0};
  PatchDrain Drain = DrainPatchRing(Header, {});
  EXPECT_TRUE(Drain.Records.empty());
}

// A ring shorter than the header's capacity claims is a truncated read, not a
// reason to walk off the end of the buffer.
TEST(PatchControlBlockTest, ToleratesARingShorterThanCapacity) {
  PatchRingHeader Header{4, 0, 8, 6};
  auto Ring = MakeRing(2, {{1, 0, 1, 10}, {1, 0, 2, 20}});
  PatchDrain Drain = DrainPatchRing(Header, Ring);
  // With Seq=4, Drained=0, Capacity=8, and only 2 records' worth of buffer:
  // Readable = min(4, 8) = 4. First = 0.
  // Loop reads sequences 0..3: slots 0,1 are in the buffer (reads succeed),
  // slots 2,3 are beyond it (skipped). So 2 records, 2 lost.
  EXPECT_EQ(2u, Drain.Records.size());
  EXPECT_EQ(2u, Drain.Lost);
}

TEST(PatchControlBlockTest, DefaultCapacityIsAPowerOfTwo) {
  EXPECT_EQ(0u, kDefaultRingCapacity & (kDefaultRingCapacity - 1));
}
