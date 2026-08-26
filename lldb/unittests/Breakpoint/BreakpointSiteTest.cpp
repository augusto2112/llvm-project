//===-- BreakpointSiteTest.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Breakpoint/BreakpointSite.h"
#include "gtest/gtest.h"

using namespace lldb_private;

// The three existing types are distinguished by who owns the trap: lldb wrote
// it, the hardware holds it, or a remote stub manages it. A trap the program
// itself contains is a fourth answer, so it needs a value of its own rather
// than reusing one whose enable path would install a trap.
TEST(BreakpointSiteTest, ProgramTrapIsItsOwnType) {
  EXPECT_NE(BreakpointSite::eProgramTrap, BreakpointSite::eSoftware);
  EXPECT_NE(BreakpointSite::eProgramTrap, BreakpointSite::eHardware);
  EXPECT_NE(BreakpointSite::eProgramTrap, BreakpointSite::eExternal);
}
