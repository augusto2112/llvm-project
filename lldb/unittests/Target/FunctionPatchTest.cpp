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
                              PatchFailure::CompileFailed,
                              PatchFailure::CaptureNotScalar,
                              PatchFailure::Unsupported};
  for (PatchFailure Reason : All) {
    EXPECT_FALSE(ToString(Reason).empty());
    EXPECT_NE("unknown", ToString(Reason));
  }
}
