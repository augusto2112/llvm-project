//===-- DWIMValueResolutionTest.cpp ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/DWIMValueResolution.h"
#include "gtest/gtest.h"

using namespace lldb_private;

TEST(DWIMValueResolutionTest, DotOnlyModeMatchesDWIMPrint) {
  // dwim-print deliberately refuses these, because -> and [] can be
  // overloaded in C++ and * and & are ambiguous.
  EXPECT_TRUE(IsVariablePathEligible("i", false));
  EXPECT_TRUE(IsVariablePathEligible("a.b.c", false));
  EXPECT_FALSE(IsVariablePathEligible("p->x", false));
  EXPECT_FALSE(IsVariablePathEligible("a[0]", false));
  EXPECT_FALSE(IsVariablePathEligible("*p", false));
  EXPECT_FALSE(IsVariablePathEligible("&a", false));

  // A register is reachable as a path, and must stay that way in both modes.
  EXPECT_TRUE(IsVariablePathEligible("$rax", false));
  EXPECT_TRUE(IsVariablePathEligible("$rax", true));
}

TEST(DWIMValueResolutionTest, PointerPathModeAllowsArrowAndSubscript) {
  EXPECT_TRUE(IsVariablePathEligible("p->x", true));
  EXPECT_TRUE(IsVariablePathEligible("a[0]", true));
  EXPECT_TRUE(IsVariablePathEligible("I->Ops[0].Ty", true));
  // Deref and address-of stay out: they are ambiguous either way.
  EXPECT_FALSE(IsVariablePathEligible("*p", true));
  EXPECT_FALSE(IsVariablePathEligible("&a", true));
  // Admitting `->` must not admit either of its characters on its own.
  EXPECT_FALSE(IsVariablePathEligible("a-b", true));
  EXPECT_FALSE(IsVariablePathEligible("a - 1", true));
  EXPECT_FALSE(IsVariablePathEligible("a > b", true));
  EXPECT_FALSE(IsVariablePathEligible("a<b", true));
  EXPECT_FALSE(IsVariablePathEligible("p-", true));
  EXPECT_FALSE(IsVariablePathEligible(">p", true));
}

TEST(DWIMValueResolutionTest, CallsAreNeverPaths) {
  // A function call must reach the expression evaluator in both modes.
  EXPECT_FALSE(IsVariablePathEligible("f()", false));
  EXPECT_FALSE(IsVariablePathEligible("f()", true));
  EXPECT_FALSE(IsVariablePathEligible("I->getName()", true));
  EXPECT_FALSE(IsVariablePathEligible("a + 1", true));
  EXPECT_FALSE(IsVariablePathEligible("a == 4000", true));
}

TEST(DWIMValueResolutionTest, EmptyIsNotEligible) {
  EXPECT_FALSE(IsVariablePathEligible("", false));
  EXPECT_FALSE(IsVariablePathEligible("", true));
}

TEST(DWIMValueResolutionTest, TierNamesAreStable) {
  EXPECT_EQ(ToString(ValueResolutionTier::VariablePath), "path");
  EXPECT_EQ(ToString(ValueResolutionTier::PersistentVariable), "persistent");
  EXPECT_EQ(ToString(ValueResolutionTier::Expression), "expression");
  EXPECT_EQ(ToString(ValueResolutionTier::Unresolved), "unavailable");
}
