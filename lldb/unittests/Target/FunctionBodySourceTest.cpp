//===-- FunctionBodySourceTest.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/FunctionBodySource.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"

using namespace lldb_private;

static llvm::Expected<FunctionBodyText> Extract(llvm::StringRef Buffer,
                                                uint32_t DeclLine) {
  return ExtractFunctionBody(Buffer, DeclLine);
}

TEST(FunctionBodySourceTest, ExtractsASimpleFunction) {
  llvm::StringRef Buffer = "int a;\n"
                           "int f(int x) {\n"
                           "  return x + 1;\n"
                           "}\n"
                           "int b;\n";
  auto Body = Extract(Buffer, 2);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_EQ("int f(int x) {\n  return x + 1;\n}", Body->Text);
  EXPECT_EQ(2u, Body->FirstLine);
}

TEST(FunctionBodySourceTest, IndexesEachLine) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  int y = 0;\n"
                           "  return y;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  ASSERT_EQ(4u, Body->LineStarts.size());
  EXPECT_EQ(0u, Body->LineStarts[0]);
  EXPECT_EQ("  int y = 0;\n  return y;\n}",
            Body->Text.substr(Body->LineStarts[1]));
  EXPECT_EQ("  return y;\n}", Body->Text.substr(Body->LineStarts[2]));
  EXPECT_EQ("}", Body->Text.substr(Body->LineStarts[3]));
}

TEST(FunctionBodySourceTest, CountsNestedBraces) {
  llvm::StringRef Buffer = "int f(int x) {\n"
                           "  if (x) {\n"
                           "    x = 2;\n"
                           "  }\n"
                           "  return x;\n"
                           "}\n"
                           "int after;\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_TRUE(llvm::StringRef(Body->Text).ends_with("return x;\n}"));
}

// A brace inside a string literal closes nothing. Without this the body would
// end early and the generated source would not compile.
TEST(FunctionBodySourceTest, IgnoresBracesInStringLiterals) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  puts(\"}\");\n"
                           "  return 0;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_TRUE(llvm::StringRef(Body->Text).ends_with("return 0;\n}"));
}

TEST(FunctionBodySourceTest, IgnoresBracesInCharLiterals) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  char c = '}';\n"
                           "  return c;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_TRUE(llvm::StringRef(Body->Text).ends_with("return c;\n}"));
}

TEST(FunctionBodySourceTest, IgnoresEscapedQuoteInsideString) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  puts(\"\\\"}\");\n"
                           "  return 0;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_TRUE(llvm::StringRef(Body->Text).ends_with("return 0;\n}"));
}

// An even run of backslashes leaves the quote unescaped, so the string ends
// there and the brace after it is code. The odd case is covered above; this is
// the boundary the index arithmetic sits on.
TEST(FunctionBodySourceTest, AnEvenBackslashRunDoesNotEscapeTheQuote) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  puts(\"a\\\\\");\n"
                           "  return 0;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_TRUE(llvm::StringRef(Body->Text).ends_with("return 0;\n}"));
}

TEST(FunctionBodySourceTest, IgnoresBracesInLineComments) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  // }\n"
                           "  return 0;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_TRUE(llvm::StringRef(Body->Text).ends_with("return 0;\n}"));
}

TEST(FunctionBodySourceTest, IgnoresBracesInBlockComments) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  /* } */\n"
                           "  return 0;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_TRUE(llvm::StringRef(Body->Text).ends_with("return 0;\n}"));
}

// C has no nested comments: a `/*` inside a line comment is text, and the
// comment still ends at the newline. Written with nothing that would close a
// block comment later in the body, so a scanner that opened one here would run
// to the end of the buffer and never balance the braces.
TEST(FunctionBodySourceTest, ABlockOpenerInsideALineCommentIsText) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  // /*\n"
                           "  return 0;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_TRUE(llvm::StringRef(Body->Text).ends_with("return 0;\n}"));
}

// And the other way round: a `//` inside a block comment is text, so the block
// runs to its `*/` and the brace in between is not the body's. A scanner that
// took the `//` for a line comment would leave that brace as code and end the
// body at it -- succeeding, with the wrong text, which is why the text is what
// is asserted here rather than the success.
TEST(FunctionBodySourceTest, ALineOpenerInsideABlockCommentIsText) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  /* //\n"
                           "     } */\n"
                           "  return 0;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_TRUE(llvm::StringRef(Body->Text).ends_with("return 0;\n}"));
}

// A body of one line, whose closing brace is the buffer's last byte with no
// newline after it. Two boundaries at once: the line index has nothing to add
// past the brace, and the scan has to stop without reading past the end.
TEST(FunctionBodySourceTest, HandlesASingleLineBodyEndingTheBuffer) {
  llvm::StringRef Buffer = "int f(void) { return 0; }";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_EQ("int f(void) { return 0; }", Body->Text);
  EXPECT_EQ(1u, Body->FirstLine);
  ASSERT_EQ(1u, Body->LineStarts.size());
  EXPECT_EQ(0u, Body->LineStarts[0]);
}

// The copy would get storage of its own, diverging from the original's, so a
// body that keeps state between calls is refused rather than silently changed.
TEST(FunctionBodySourceTest, RefusesAStaticLocal) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  static int n = 0;\n"
                           "  return ++n;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  EXPECT_THAT_EXPECTED(Body, llvm::Failed());
}

// `static` naming a type or appearing inside a string is not storage.
TEST(FunctionBodySourceTest, AllowsStaticAsPartOfAnotherWord) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  int statics = 1;\n"
                           "  return statics;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  EXPECT_THAT_EXPECTED(Body, llvm::Succeeded());
}

// `static` on the declaration gives the function internal linkage, which says
// nothing about whether a copy would behave differently. Refusing it would
// refuse most of the C worth patching.
TEST(FunctionBodySourceTest, AllowsAStaticFunction) {
  llvm::StringRef Buffer = "static int f(int x) {\n"
                           "  return x + 1;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_EQ("static int f(int x) {\n  return x + 1;\n}", Body->Text);
}

// A static local inside a static function is still refused: the storage is what
// the copy cannot share, and the linkage is beside the point.
TEST(FunctionBodySourceTest, RefusesAStaticLocalInAStaticFunction) {
  llvm::StringRef Buffer = "static int f(void) {\n"
                           "  static int n = 0;\n"
                           "  return ++n;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  EXPECT_THAT_EXPECTED(Body, llvm::Failed());
}

// A brace inside a comment closes nothing, so a `static` inside one declares
// nothing either.
// `static` with no whitespace between it and the comment that precedes it. The
// detection compares the bytes around the word, so this is the one boundary
// where an off-by-one would silently stop refusing a static local.
TEST(FunctionBodySourceTest, RefusesAStaticLocalAbuttingAComment) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  /* keep */static int n = 0;\n"
                           "  return ++n;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  EXPECT_THAT_EXPECTED(Body, llvm::Failed());
}

TEST(FunctionBodySourceTest, IgnoresStaticInAComment) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  // static int n = 0;\n"
                           "  return 0;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  EXPECT_THAT_EXPECTED(Body, llvm::Succeeded());
}

TEST(FunctionBodySourceTest, FailsWhenBracesNeverBalance) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  return 0;\n";
  auto Body = Extract(Buffer, 1);
  EXPECT_THAT_EXPECTED(Body, llvm::Failed());
}

TEST(FunctionBodySourceTest, FailsWhenNoBraceFollowsTheDeclaration) {
  llvm::StringRef Buffer = "int f(void);\n";
  auto Body = Extract(Buffer, 1);
  EXPECT_THAT_EXPECTED(Body, llvm::Failed());
}

TEST(FunctionBodySourceTest, FailsWhenTheLineIsPastTheBuffer) {
  llvm::StringRef Buffer = "int f(void) {}\n";
  auto Body = Extract(Buffer, 99);
  EXPECT_THAT_EXPECTED(Body, llvm::Failed());
}

// A declaration spanning several lines before its brace is ordinary in
// formatted code, so the search for the opening brace cannot stop at the
// declaration's own line.
TEST(FunctionBodySourceTest, HandlesAMultiLineDeclaration) {
  llvm::StringRef Buffer = "int f(int a,\n"
                           "      int b)\n"
                           "{\n"
                           "  return a + b;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_EQ(1u, Body->FirstLine);
  EXPECT_TRUE(llvm::StringRef(Body->Text).ends_with("return a + b;\n}"));
}
