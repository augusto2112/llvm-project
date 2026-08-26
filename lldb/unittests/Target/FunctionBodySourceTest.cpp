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
//
// It is blanked rather than kept: the expression parser derives a declaration of
// the function from the program's own debug info, where it has external linkage,
// and a definition that follows it with internal linkage is one the compiler
// refuses. Blanked in place, so every byte after it keeps its offset and line.
TEST(FunctionBodySourceTest, BlanksStaticOnTheDeclaration) {
  llvm::StringRef Buffer = "static int f(int x) {\n"
                           "  return x + 1;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_EQ("       int f(int x) {\n  return x + 1;\n}", Body->Text);
}

TEST(FunctionBodySourceTest, BlanksStaticInlineOnTheDeclaration) {
  llvm::StringRef Buffer = "static inline int f(void) {\n"
                           "  return 1;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_EQ("              int f(void) {\n  return 1;\n}", Body->Text);
}

// On a line of its own, which is where a signature broken across lines puts it,
// so the blanking has to reach text taken from above the name's line.
TEST(FunctionBodySourceTest, BlanksStaticAboveTheNamesLine) {
  llvm::StringRef Buffer = "static int\n"
                           "f(int x) {\n"
                           "  return x;\n"
                           "}\n";
  auto Body = Extract(Buffer, 2);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_EQ(1u, Body->FirstLine);
  EXPECT_EQ("       int\nf(int x) {\n  return x;\n}", Body->Text);
}

// `static` as a parameter's array bound is C99's "at least this many", which is
// part of the parameter's type rather than the definition's linkage.
TEST(FunctionBodySourceTest, KeepsStaticInsideTheParameterList) {
  llvm::StringRef Buffer = "int f(int a[static 4]) {\n"
                           "  return a[0];\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_EQ("int f(int a[static 4]) {\n  return a[0];\n}", Body->Text);
}

// And a name that merely contains one of the keywords is not one.
TEST(FunctionBodySourceTest, KeepsAWordEndingInAKeyword) {
  llvm::StringRef Buffer = "int mystatic(int x) {\n"
                           "  return x;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_EQ("int mystatic(int x) {\n  return x;\n}", Body->Text);
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

// `DW_AT_decl_line` names the line the function's name is on, which for a
// signature broken across lines is not where the declaration starts. Text taken
// from the name onwards declares nothing -- it has no return type -- so the
// lines above it are part of what has to be recompiled.
TEST(FunctionBodySourceTest, TakesAReturnTypeFromTheLineAbove) {
  llvm::StringRef Buffer = "int before;\n"
                           "\n"
                           "static int\n"
                           "f(int a,\n"
                           "  int b)\n"
                           "{\n"
                           "  return a + b;\n"
                           "}\n";
  auto Body = Extract(Buffer, 4);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_EQ(3u, Body->FirstLine);
  EXPECT_TRUE(llvm::StringRef(Body->Text).starts_with("       int\nf(int a,"));
  EXPECT_TRUE(llvm::StringRef(Body->Text).ends_with("return a + b;\n}"));
  // The index still starts at the text's own first line, which is what places
  // an injection: line `FirstLine + N` begins at `LineStarts[N]`.
  ASSERT_EQ(6u, Body->LineStarts.size());
  EXPECT_EQ("  return a + b;\n}", Body->Text.substr(Body->LineStarts[4]));
}

// Several such lines, since an attribute and a return type can each have one of
// their own.
TEST(FunctionBodySourceTest, TakesEveryLineThatOnlyLeadsIntoTheName) {
  llvm::StringRef Buffer = "\n"
                           "__attribute__((noinline))\n"
                           "static const char *\n"
                           "f(void)\n"
                           "{\n"
                           "  return \"x\";\n"
                           "}\n";
  auto Body = Extract(Buffer, 4);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_EQ(2u, Body->FirstLine);
  EXPECT_TRUE(
      llvm::StringRef(Body->Text).starts_with("__attribute__((noinline))\n"));
}

// The line above ends a declaration of something else, so it is that
// declaration's rather than this one's.
TEST(FunctionBodySourceTest, StopsAtAStatementAbove) {
  llvm::StringRef Buffer = "int before;\n"
                           "int f(int a) {\n"
                           "  return a;\n"
                           "}\n";
  auto Body = Extract(Buffer, 2);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_EQ(2u, Body->FirstLine);
  EXPECT_EQ("int f(int a) {\n  return a;\n}", Body->Text);
}

// And so does the closing brace of whatever came before.
TEST(FunctionBodySourceTest, StopsAtAClosingBraceAbove) {
  llvm::StringRef Buffer = "int g(void) {\n"
                           "  return 0;\n"
                           "}\n"
                           "int\n"
                           "f(int a) {\n"
                           "  return a;\n"
                           "}\n";
  auto Body = Extract(Buffer, 5);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_EQ(4u, Body->FirstLine);
  EXPECT_TRUE(llvm::StringRef(Body->Text).starts_with("int\nf(int a) {"));
}

// A directive above is the preprocessor's, and taking it into a body that is
// compiled on its own would declare it a second time.
TEST(FunctionBodySourceTest, StopsAtAPreprocessorDirectiveAbove) {
  llvm::StringRef Buffer = "#define N 1\n"
                           "int f(void) {\n"
                           "  return N;\n"
                           "}\n";
  auto Body = Extract(Buffer, 2);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_EQ(2u, Body->FirstLine);
  EXPECT_EQ("int f(void) {\n  return N;\n}", Body->Text);
}

// A comment above is not taken either. It would be harmless to recompile, but a
// comment can be opened on one line and closed on another, and a start chosen
// part way through one would leave text that closes a comment nothing opened.
TEST(FunctionBodySourceTest, StopsAtACommentAbove) {
  llvm::StringRef Buffer = "/* what f does:\n"
                           " * it returns a; that is all\n"
                           " */\n"
                           "int\n"
                           "f(int a) {\n"
                           "  return a;\n"
                           "}\n";
  auto Body = Extract(Buffer, 5);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_EQ(4u, Body->FirstLine);
  EXPECT_EQ("int\nf(int a) {\n  return a;\n}", Body->Text);
}

// The first line of the buffer, with nothing above it to stop at.
TEST(FunctionBodySourceTest, TakesTheReturnTypeOnTheFirstLineOfTheBuffer) {
  llvm::StringRef Buffer = "int\n"
                           "f(void) {\n"
                           "  return 0;\n"
                           "}\n";
  auto Body = Extract(Buffer, 2);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_EQ(1u, Body->FirstLine);
  EXPECT_EQ("int\nf(void) {\n  return 0;\n}", Body->Text);
}
