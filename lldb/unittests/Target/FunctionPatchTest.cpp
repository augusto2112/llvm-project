//===-- FunctionPatchTest.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/FunctionPatch.h"
#include "gtest/gtest.h"
#include <chrono>
#include <cstring>

using namespace lldb_private;
using llvm::sys::TimePoint;

static TimePoint<> At(long Seconds) {
  return TimePoint<>() + std::chrono::seconds(Seconds);
}

// A build writes the binary after the sources it read, and a filesystem with
// coarse timestamps can leave the two within a second of each other either way.
// Refusing on that noise would refuse every ordinary build.
TEST(FunctionPatchTest, IgnoresSkewWithinTheNoiseFloor) {
  EXPECT_FALSE(SourceSkewExceedsNoise(At(1000), At(1000)));
  EXPECT_FALSE(SourceSkewExceedsNoise(At(1001), At(1000)));
}

TEST(FunctionPatchTest, DetectsSourceWrittenAfterTheBinary) {
  EXPECT_TRUE(SourceSkewExceedsNoise(At(1010), At(1000)));
}

TEST(FunctionPatchTest, IgnoresSourceOlderThanTheBinary) {
  EXPECT_FALSE(SourceSkewExceedsNoise(At(1000), At(1010)));
}

// An unreadable mtime arrives as the epoch. A comparison that cannot be made is
// not a finding, and refusing on it would refuse every binary built elsewhere.
TEST(FunctionPatchTest, IgnoresAnUnreadableTimestamp) {
  EXPECT_FALSE(SourceSkewExceedsNoise(TimePoint<>(), At(1000)));
  EXPECT_FALSE(SourceSkewExceedsNoise(At(1000), TimePoint<>()));
}

// Every reason is nameable, because a report that says a patch was refused
// without saying why leaves the caller unable to act on it.
TEST(FunctionPatchTest, NamesEveryFailure) {
  const PatchFailure All[] = {PatchFailure::NotArm64,
                              PatchFailure::NoProcess,
                              PatchFailure::InferiorAccessFailed,
                              PatchFailure::NoSourceFile,
                              PatchFailure::SourceNewerThanBinary,
                              PatchFailure::BodyNotFound,
                              PatchFailure::StaticLocal,
                              PatchFailure::EntryTooSmall,
                              PatchFailure::ThreadInPatchRange,
                              PatchFailure::BreakpointInPatchRange,
                              PatchFailure::BreakpointInRedirectedBody,
                              PatchFailure::CompileFailed,
                              PatchFailure::CaptureNotScalar,
                              PatchFailure::Unsupported};
  for (PatchFailure Reason : All) {
    EXPECT_FALSE(ToString(Reason).empty());
    EXPECT_NE("unknown", ToString(Reason));
  }
}

TEST(FunctionPatchTest, ReadsANarrowCaptureWithoutItsPadding) {
  // A one-byte capture arrives zero-extended into eight. Reading all eight
  // would report seven bytes of padding as part of the value.
  auto Bytes = CaptureValueBytes(0xFF, 1, lldb::eByteOrderLittle);
  ASSERT_EQ(1u, Bytes.size());
  EXPECT_EQ(0xFF, Bytes[0]);
}

TEST(FunctionPatchTest, ReadsAFourByteCaptureWithoutTheHighHalf) {
  auto Bytes = CaptureValueBytes(0x00000000AABBCCDD, 4, lldb::eByteOrderLittle);
  ASSERT_EQ(4u, Bytes.size());
  EXPECT_EQ(0xDD, Bytes[0]);
  EXPECT_EQ(0xAA, Bytes[3]);
}

// A double reaches the record through a memcpy rather than a cast, so its bits
// are the bits the program held. They have to survive the trip back too.
TEST(FunctionPatchTest, RoundTripsADoublesBits) {
  const double Original = -1.5e-300;
  uint64_t Raw = 0;
  std::memcpy(&Raw, &Original, sizeof Raw);
  auto Bytes = CaptureValueBytes(Raw, sizeof(double), lldb::eByteOrderLittle);
  ASSERT_EQ(8u, Bytes.size());
  double Back = 0;
  std::memcpy(&Back, Bytes.data(), sizeof Back);
  EXPECT_EQ(Original, Back);
}

TEST(FunctionPatchTest, OrdersBytesForABigEndianReader) {
  auto Bytes = CaptureValueBytes(0x0000000000ABCDEF, 4, lldb::eByteOrderBig);
  ASSERT_EQ(4u, Bytes.size());
  EXPECT_EQ(0x00, Bytes[0]);
  EXPECT_EQ(0xEF, Bytes[3]);
}

// A type wider than the field cannot have fitted through it, so clamping is
// what keeps a bad type from reading past the record.
TEST(FunctionPatchTest, ClampsAWidthWiderThanTheField) {
  auto Bytes = CaptureValueBytes(~0ull, 16, lldb::eByteOrderLittle);
  EXPECT_EQ(8u, Bytes.size());
}
