//===-- TestSwiftDWARFValidationJournal.cpp -------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2026 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "Plugins/LanguageRuntime/Swift/SwiftDWARFValidationJournal.h"
#include "swift/RemoteInspection/TypeLowering.h"
#include "gtest/gtest.h"

using namespace lldb_private::swift_dwarf_journal;
using namespace swift::reflection;

// A strong/native class reference on both sides, diverging only on the number
// of extra inhabitants (the real `a.Foo` divergence: refl 2147483647 vs 1).
static ReferenceTypeInfo makeRef(unsigned xi) {
  return ReferenceTypeInfo(/*Size=*/8, /*Alignment=*/8, /*Stride=*/8,
                           /*NumExtraInhabitants=*/xi,
                           BitwiseBorrowability::TakableAndBorrowable,
                           ReferenceKind::Strong, ReferenceCounting::Native);
}

TEST(SwiftDWARFValidationJournal, AllDifferencesReportsExtraInhabitants) {
  auto refl = makeRef(2147483647);
  auto dwarf = makeRef(1);
  auto diffs = allTypeInfoDifferences(refl, dwarf, TypeInfoComparison::Strict);
  ASSERT_EQ(diffs.size(), 1u);
  EXPECT_EQ(diffs[0].dimension, "num_extra_inhabitants");
  EXPECT_EQ(diffs[0].refl, "2147483647");
  EXPECT_EQ(diffs[0].dwarf, "1");
}

TEST(SwiftDWARFValidationJournal, AllDifferencesRespectsFlags) {
  // XI differs, but Layout excludes NumExtraInhabitants -> no difference.
  auto a = makeRef(2147483647);
  auto b = makeRef(1);
  auto diffs = allTypeInfoDifferences(a, b, TypeInfoComparison::Layout);
  EXPECT_TRUE(diffs.empty());
}

TEST(SwiftDWARFValidationJournal, AllDifferencesEqualIsEmpty) {
  auto a = makeRef(1);
  auto b = makeRef(1);
  EXPECT_TRUE(allTypeInfoDifferences(a, b, TypeInfoComparison::Strict).empty());
}

TEST(SwiftDWARFValidationJournal, AllDifferencesDetectsKindMismatch) {
  auto ref = makeRef(0);
  BuiltinTypeInfo bi(/*Size=*/8, /*Alignment=*/8, /*Stride=*/8,
                     /*NumExtraInhabitants=*/0,
                     BitwiseBorrowability::TakableAndBorrowable,
                     /*AddressableForDependencies=*/false);
  auto diffs = allTypeInfoDifferences(ref, bi, TypeInfoComparison::Strict);
  ASSERT_EQ(diffs.size(), 1u);
  EXPECT_EQ(diffs[0].dimension, "kind");
}

TEST(SwiftDWARFValidationJournal, StringVocabulary) {
  auto ref = makeRef(0);
  EXPECT_EQ(typeInfoKindString(ref), "reference");
  EXPECT_EQ(referenceKindString(ReferenceKind::Strong), "strong");
  EXPECT_EQ(referenceCountingString(ReferenceCounting::Native), "native");
}
