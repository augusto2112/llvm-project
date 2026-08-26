//===-- PatchSourceBuilderTest.cpp -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/PatchSourceBuilder.h"
#include "lldb/Target/FunctionBodySource.h"
#include "gtest/gtest.h"

using namespace lldb_private;

/// The body used throughout: `f` declared on line 4, one statement on line 5,
/// a return on line 6, closing brace on line 7.
static FunctionBodyText Body() {
  llvm::StringRef Buffer = "a\nb\nc\n"
                           "int f(int x) {\n"
                           "  int acc = x;\n"
                           "  return acc;\n"
                           "}\n";
  auto Extracted = ExtractFunctionBody(Buffer, 4);
  return cantFail(std::move(Extracted));
}

static PatchSourceRequest Request(std::vector<PatchInjection> Injections) {
  PatchSourceRequest Req;
  Req.Body = Body();
  Req.SourcePath = "/tmp/t.c";
  Req.RingAddress = 0x104000000;
  Req.RingCapacity = 4096;
  Req.Injections = std::move(Injections);
  return Req;
}

static PatchInjection Bare(uint32_t Line) {
  PatchInjection Inj;
  Inj.SiteID = 1;
  Inj.Line = Line;
  Inj.SlotAddress = 0x104010018;
  Inj.WantStop = true;
  return Inj;
}

TEST(PatchSourceBuilderTest, EmitsTheBodyVerbatimWithNoInjections) {
  std::string Source = BuildPatchSource(Request({}));
  EXPECT_NE(std::string::npos, Source.find("int f(int x) {\n"
                                           "  int acc = x;\n"
                                           "  return acc;\n"
                                           "}"));
}

TEST(PatchSourceBuilderTest, OpensTheBodyWithItsOwnFirstLine) {
  std::string Source = BuildPatchSource(Request({}));
  EXPECT_NE(std::string::npos,
            Source.find("#line 4 \"/tmp/t.c\"\nint f(int x) {"));
}

TEST(PatchSourceBuilderTest, BakesTheRingAddressAsALiteral) {
  std::string Source = BuildPatchSource(Request({}));
  EXPECT_NE(std::string::npos, Source.find("0x104000000"));
}

TEST(PatchSourceBuilderTest, BakesTheSiteSlotAddressAsALiteral) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("0x104010018"));
}

// Both the injected line and the statement it precedes claim the same line, so
// a stop just past the trap reports the line the caller asked about rather than
// the next one.
TEST(PatchSourceBuilderTest, BracketsAnInjectionWithMatchingLineDirectives) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  std::string Source = BuildPatchSource(Request({Inj}));
  size_t Before = Source.find("#line 5 \"/tmp/t.c\"\n");
  ASSERT_NE(std::string::npos, Before);
  size_t After = Source.find("#line 5 \"/tmp/t.c\"\n", Before + 1);
  ASSERT_NE(std::string::npos, After);
  EXPECT_NE(std::string::npos, Source.find("  int acc = x;", After));
}

// One physical line is what keeps the accounting trivial: two directives and
// one line between them, so no later line shifts.
TEST(PatchSourceBuilderTest, EmitsAnInjectionOnOnePhysicalLine) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  Inj.Captures = {"acc"};
  std::string Source = BuildPatchSource(Request({Inj}));
  size_t Start = Source.find("#line 5 \"/tmp/t.c\"\n");
  ASSERT_NE(std::string::npos, Start);
  Start += std::string("#line 5 \"/tmp/t.c\"\n").size();
  size_t End = Source.find('\n', Start);
  ASSERT_NE(std::string::npos, End);
  std::string Line = Source.substr(Start, End - Start);
  EXPECT_NE(std::string::npos, Line.find("__builtin_debugtrap()"));
  EXPECT_NE(std::string::npos, Line.find("__lldb_rec"));
}

TEST(PatchSourceBuilderTest, GuardsOnTheConditionWhenThereIsOne) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("if (acc > 1)"));
}

// A site with no condition still counts hits and still records, so the absence
// of a condition must not produce an `if` with nothing in it.
TEST(PatchSourceBuilderTest, OmitsTheConditionGuardWhenThereIsNone) {
  auto Inj = Bare(5);
  Inj.Captures = {"acc"};
  Inj.WantStop = false;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_EQ(std::string::npos, Source.find("if ()"));
  EXPECT_NE(std::string::npos, Source.find("__lldb_rec"));
}

TEST(PatchSourceBuilderTest, CountsHitsBeforeTestingTheCondition) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  std::string Source = BuildPatchSource(Request({Inj}));
  size_t Hits = Source.find("->hits");
  size_t Cond = Source.find("if (acc > 1)");
  ASSERT_NE(std::string::npos, Hits);
  ASSERT_NE(std::string::npos, Cond);
  EXPECT_LT(Hits, Cond);
}

TEST(PatchSourceBuilderTest, CountsConditionTruthInsideTheCondition) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  std::string Source = BuildPatchSource(Request({Inj}));
  size_t Cond = Source.find("if (acc > 1)");
  size_t True = Source.find("->cond_true");
  ASSERT_NE(std::string::npos, True);
  EXPECT_LT(Cond, True);
}

TEST(PatchSourceBuilderTest, ComparesTheHitCountAgainstSkipFirst) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  Inj.SkipFirst = 10;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("> 10"));
}

TEST(PatchSourceBuilderTest, ComparesTheHitCountForEqualityForOnlyHit) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  Inj.OnlyHit = 3;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("== 3"));
}

TEST(PatchSourceBuilderTest, ReadsTheGateWhenGated) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  Inj.Gated = true;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("->gate"));
}

TEST(PatchSourceBuilderTest, OmitsTheGateWhenNotGated) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  Inj.Gated = false;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_EQ(std::string::npos, Source.find("->gate"));
}

// A cast would truncate a double. The copy is bit-exact for every scalar
// because it is a memcpy of the value's own width.
TEST(PatchSourceBuilderTest, CopiesACaptureByItsOwnWidth) {
  auto Inj = Bare(5);
  Inj.Captures = {"acc"};
  Inj.WantStop = false;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("__typeof__(acc) __lldb_cap_1_0"));
  EXPECT_NE(std::string::npos,
            Source.find("__builtin_memcpy(&__lldb_v_1_0, &__lldb_cap_1_0, "
                        "sizeof __lldb_cap_1_0)"));
}

TEST(PatchSourceBuilderTest, NamesEachCaptureLocalDistinctly) {
  auto Inj = Bare(5);
  Inj.Captures = {"acc", "x"};
  Inj.WantStop = false;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("__lldb_cap_1_0"));
  EXPECT_NE(std::string::npos, Source.find("__lldb_cap_1_1"));
  EXPECT_EQ("__lldb_cap_1_0", CaptureLocalName(1, 0));
  EXPECT_EQ("__lldb_cap_1_1", CaptureLocalName(1, 1));
}

TEST(PatchSourceBuilderTest, TrapsWhenTheSiteWantsAStop) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  Inj.WantStop = true;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("__builtin_debugtrap()"));
}

// A site that only records must not stop, or every recorded value would cost
// the stop the recording exists to avoid.
TEST(PatchSourceBuilderTest, DoesNotTrapWhenTheSiteOnlyRecords) {
  auto Inj = Bare(5);
  Inj.Captures = {"acc"};
  Inj.WantStop = false;
  std::string Source = BuildPatchSource(Request({Inj}));
  // The only trap left is the drain trap, which tests the ring, not the
  // condition.
  size_t First = Source.find("__builtin_debugtrap()");
  ASSERT_NE(std::string::npos, First);
  EXPECT_NE(std::string::npos, Source.rfind("high_water", First));
}

TEST(PatchSourceBuilderTest, EmitsADrainTrapWhenThereAreCaptures) {
  auto Inj = Bare(5);
  Inj.Captures = {"acc"};
  Inj.WantStop = false;
  std::string Source = BuildPatchSource(Request({Inj}));
  // The comparison, not the field name: the header declares `high_water`
  // whether or not anything reads it.
  EXPECT_NE(std::string::npos, Source.find("- __LLDB_HDR->drained >="));
}

// Nothing is recorded, so nothing can fill the ring, so asking whether it is
// full would cost two loads per hit for an answer that cannot change.
TEST(PatchSourceBuilderTest, OmitsTheDrainTrapWithNoCaptures) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_EQ(std::string::npos, Source.find("- __LLDB_HDR->drained >="));
}

// The header's shape is a contract with the struct the debugger reads the block
// with, so it cannot depend on what any one patch happens to need. Omitting a
// field it does not read would move `ring` and cost nothing, since a
// declaration is not storage.
TEST(PatchSourceBuilderTest, DeclaresTheWholeHeaderWhateverThePatchUses) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  std::string WithoutCaptures = BuildPatchSource(Request({Inj}));
  Inj.Captures = {"acc"};
  std::string WithCaptures = BuildPatchSource(Request({Inj}));

  const char *Decl = "struct __lldb_hdr_t { unsigned long seq, drained, "
                     "capacity, high_water; struct __lldb_rec_t ring[]; };";
  EXPECT_NE(std::string::npos, WithoutCaptures.find(Decl));
  EXPECT_NE(std::string::npos, WithCaptures.find(Decl));
}

TEST(PatchSourceBuilderTest, PlacesTwoInjectionsAtTheirOwnLines) {
  auto First = Bare(5);
  First.SiteID = 1;
  First.Condition = "acc > 1";
  auto Second = Bare(6);
  Second.SiteID = 2;
  Second.SlotAddress = 0x104010030;
  Second.Condition = "acc > 2";
  std::string Source = BuildPatchSource(Request({First, Second}));
  size_t One = Source.find("if (acc > 1)");
  size_t Two = Source.find("if (acc > 2)");
  size_t Stmt = Source.find("  int acc = x;");
  size_t Ret = Source.find("  return acc;");
  ASSERT_NE(std::string::npos, One);
  ASSERT_NE(std::string::npos, Two);
  EXPECT_LT(One, Stmt);
  EXPECT_LT(Stmt, Two);
  EXPECT_LT(Two, Ret);
}

// Two conditions on the same line is what an already-patched function looks
// like when a second condition arrives at the same place.
TEST(PatchSourceBuilderTest, PlacesTwoInjectionsOnTheSameLine) {
  auto First = Bare(5);
  First.SiteID = 1;
  First.Condition = "acc > 1";
  auto Second = Bare(5);
  Second.SiteID = 2;
  Second.SlotAddress = 0x104010030;
  Second.Condition = "acc > 2";
  std::string Source = BuildPatchSource(Request({First, Second}));
  size_t One = Source.find("if (acc > 1)");
  size_t Two = Source.find("if (acc > 2)");
  size_t Stmt = Source.find("  int acc = x;");
  ASSERT_NE(std::string::npos, One);
  ASSERT_NE(std::string::npos, Two);
  EXPECT_LT(One, Two);
  EXPECT_LT(Two, Stmt);
}

// An injection is emitted in line order regardless of the order it was
// installed in, because it is spliced into text that only reads forwards.
TEST(PatchSourceBuilderTest, OrdersInjectionsByLineNotByArrival) {
  auto Late = Bare(6);
  Late.SiteID = 2;
  Late.Condition = "acc > 2";
  auto Early = Bare(5);
  Early.SiteID = 1;
  Early.Condition = "acc > 1";
  std::string Source = BuildPatchSource(Request({Late, Early}));
  EXPECT_LT(Source.find("if (acc > 1)"), Source.find("if (acc > 2)"));
}

// The declaration line and the closing brace are not statements, so an
// injection there has nowhere valid to go.
TEST(PatchSourceBuilderTest, DropsAnInjectionOutsideTheBody) {
  auto Before = Bare(1);
  Before.Condition = "acc > 1";
  auto After = Bare(99);
  After.SiteID = 2;
  After.Condition = "acc > 2";
  std::string Source = BuildPatchSource(Request({Before, After}));
  EXPECT_EQ(std::string::npos, Source.find("if (acc > 1)"));
  EXPECT_EQ(std::string::npos, Source.find("if (acc > 2)"));
}

TEST(PatchSourceBuilderTest, DeclaresTheRecordWriterBeforeTheBody) {
  auto Inj = Bare(5);
  Inj.Captures = {"acc"};
  Inj.WantStop = false;
  std::string Source = BuildPatchSource(Request({Inj}));
  size_t Helper = Source.find("static void __lldb_rec");
  size_t Fn = Source.find("int f(int x) {");
  ASSERT_NE(std::string::npos, Helper);
  ASSERT_NE(std::string::npos, Fn);
  EXPECT_LT(Helper, Fn);
}

// The capacity is a literal so the writer indexes with a mask and never loads
// it.
TEST(PatchSourceBuilderTest, MasksTheRingIndexWithALiteralCapacity) {
  auto Inj = Bare(5);
  Inj.Captures = {"acc"};
  Inj.WantStop = false;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("& 4095"));
}
