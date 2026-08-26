//===-- EntryTrampolineTest.cpp -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/EntryTrampoline.h"
#include "gtest/gtest.h"
#include <cstring>

using namespace lldb_private;

// The reference encoding, taken from clang assembling
// `ldr x16, .+8` / `br x16` for arm64.
static constexpr uint8_t kLdrX16[4] = {0x50, 0x00, 0x00, 0x58};
static constexpr uint8_t kBrX16[4] = {0x00, 0x02, 0x1f, 0xd6};

TEST(EntryTrampolineTest, EncodesLoadAndBranch) {
  auto Bytes = EncodeEntryTrampoline(0x0000000100020000);
  EXPECT_EQ(0, std::memcmp(Bytes.data(), kLdrX16, 4));
  EXPECT_EQ(0, std::memcmp(Bytes.data() + 4, kBrX16, 4));
}

TEST(EntryTrampolineTest, EmbedsTargetLittleEndian) {
  auto Bytes = EncodeEntryTrampoline(0x8877665544332211);
  const uint8_t Expected[8] = {0x11, 0x22, 0x33, 0x44,
                               0x55, 0x66, 0x77, 0x88};
  EXPECT_EQ(0, std::memcmp(Bytes.data() + 8, Expected, 8));
}

// Re-pointing a trampoline that is already installed writes only the literal,
// at this offset, so the offset has to name the same bytes the whole encoding
// puts the target in. The two disagreeing would leave the redirect branching to
// half of one address and half of another.
TEST(EntryTrampolineTest, TargetOffsetNamesTheLiteral) {
  auto Before = EncodeEntryTrampoline(0x1111111111111111);
  auto After = EncodeEntryTrampoline(0x2222222222222222);
  EXPECT_EQ(0, std::memcmp(Before.data(), After.data(),
                           kEntryTrampolineTargetOffset));
  EXPECT_EQ(kEntryTrampolineSize - kEntryTrampolineTargetOffset,
            sizeof(uint64_t));
  auto Repointed = Before;
  std::memcpy(Repointed.data() + kEntryTrampolineTargetOffset,
              After.data() + kEntryTrampolineTargetOffset, sizeof(uint64_t));
  EXPECT_EQ(After, Repointed);
}

// The literal is loaded from eight bytes past the `ldr`, so the two
// instructions and the literal must total exactly the patched width. A
// different size would mean the `ldr` reads the wrong bytes.
TEST(EntryTrampolineTest, IsSixteenBytes) {
  EXPECT_EQ(16u, kEntryTrampolineSize);
  auto Bytes = EncodeEntryTrampoline(0);
  EXPECT_EQ(16u, Bytes.size());
}

// A target of zero is still encoded rather than rejected: rejecting it here
// would put the check in the wrong place, since the caller knows whether an
// address is meaningful and this function only encodes.
TEST(EntryTrampolineTest, EncodesZeroTarget) {
  auto Bytes = EncodeEntryTrampoline(0);
  const uint8_t Expected[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(0, std::memcmp(Bytes.data() + 8, Expected, 8));
}
