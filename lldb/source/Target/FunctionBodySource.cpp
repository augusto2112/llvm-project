//===-- FunctionBodySource.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/FunctionBodySource.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"

using namespace lldb_private;

char BodyExtractError::ID = 0;

llvm::StringRef lldb_private::ToString(BodyExtractFailure Reason) {
  switch (Reason) {
  case BodyExtractFailure::NotFound:
    return "no function body found at that line";
  case BodyExtractFailure::Unbalanced:
    return "the function's braces never balanced";
  case BodyExtractFailure::StaticLocal:
    return "the body declares a static local";
  }
  return "unknown";
}

namespace {

llvm::Error Fail(BodyExtractFailure Reason) {
  return llvm::make_error<BodyExtractError>(Reason);
}

/// The offset in \p Buffer at which line \p Line begins, counting from one, or
/// npos when the buffer has fewer lines than that.
size_t OffsetOfLine(llvm::StringRef Buffer, uint32_t Line) {
  if (Line == 0)
    return llvm::StringRef::npos;
  size_t Offset = 0;
  for (uint32_t Current = 1; Current < Line; ++Current) {
    size_t Newline = Buffer.find('\n', Offset);
    if (Newline == llvm::StringRef::npos)
      return llvm::StringRef::npos;
    Offset = Newline + 1;
  }
  return Offset <= Buffer.size() ? Offset : llvm::StringRef::npos;
}

/// Whether \p Line can only be part of the declaration that follows it.
///
/// A line that ends a declaration, opens or closes a scope, holds a
/// preprocessor directive, or holds any part of a comment could stand on its
/// own, so it is something else's. What is left -- `static int`,
/// `__attribute__((noinline))`, a return type on a line of its own -- has no
/// meaning except as the start of what comes next.
bool LineOnlyLeadsIntoWhatFollows(llvm::StringRef Line) {
  return !Line.trim().empty() && Line.find_first_of(";{}#/") == llvm::StringRef::npos;
}

/// Where the declaration whose name is on the line starting at \p LineStart
/// begins.
///
/// A function's `DW_AT_decl_line` names the line its own name is on, which for
/// a signature broken across lines is not where the declaration starts: the
/// return type and the storage class are on the lines above, and text taken
/// from the name onwards is not a declaration of anything. So the lines
/// immediately above are taken as well, for as long as each of them can only be
/// leading into this one.
size_t StartOfDeclaration(llvm::StringRef Buffer, size_t LineStart) {
  size_t Start = LineStart;
  while (Start > 0) {
    // Every offset this walks to is a line start, so the byte before it is the
    // newline ending the line above.
    const size_t NewlineAbove = Start - 1;
    size_t Candidate = 0;
    if (NewlineAbove > 0) {
      const size_t Earlier = Buffer.rfind('\n', NewlineAbove - 1);
      if (Earlier != llvm::StringRef::npos)
        Candidate = Earlier + 1;
    }
    if (!LineOnlyLeadsIntoWhatFollows(Buffer.slice(Candidate, NewlineAbove)))
      break;
    Start = Candidate;
  }
  return Start;
}

/// Whether the keyword \p Keyword begins at \p I in \p Text, as a whole word.
///
/// A textual check, which is what keeps this callable before anything is
/// compiled. It over-reports in one direction only: a keyword a macro would have
/// removed is still seen here, and for both of this file's uses seeing one too
/// many is the safe answer.
bool KeywordAt(llvm::StringRef Text, size_t I, llvm::StringRef Keyword) {
  if (!Text.substr(I).starts_with(Keyword))
    return false;
  const bool StartsWord =
      I == 0 || !(llvm::isAlnum(Text[I - 1]) || Text[I - 1] == '_');
  const size_t After = I + Keyword.size();
  const bool EndsWord =
      After >= Text.size() || !(llvm::isAlnum(Text[After]) || Text[After] == '_');
  return StartsWord && EndsWord;
}

/// The keywords on a declaration that say only how the definition is linked and
/// emitted.
///
/// A copy the debugger compiles is emitted on its own and reached through a
/// redirect written over the original's entry rather than by name, so none of
/// these describes anything the copy needs -- and each of them, kept, makes the
/// copy disagree with the declaration the expression parser derives from the
/// program's own debug info, which is a definition the compiler refuses.
constexpr llvm::StringLiteral kLinkageKeywords[] = {
    llvm::StringLiteral("static"), llvm::StringLiteral("inline"),
    llvm::StringLiteral("__inline"), llvm::StringLiteral("__inline__")};

} // namespace

llvm::Expected<FunctionBodyText>
lldb_private::ExtractFunctionBody(llvm::StringRef Buffer, uint32_t DeclLine) {
  const size_t NameLine = OffsetOfLine(Buffer, DeclLine);
  if (NameLine == llvm::StringRef::npos)
    return Fail(BodyExtractFailure::NotFound);

  // The declaration may start above the line its name is on, and the copy is
  // compiled from the declaration onwards, so the text has to start where the
  // declaration does rather than where debug info found the name.
  const size_t Start = StartOfDeclaration(Buffer, NameLine);
  const uint32_t FirstLine =
      DeclLine - static_cast<uint32_t>(llvm::StringRef(Buffer)
                                           .slice(Start, NameLine)
                                           .count('\n'));

  enum { Code, InString, InChar, InLineComment, InBlockComment } State = Code;
  int Depth = 0;
  int ParenDepth = 0;
  bool SawBrace = false;
  bool SawStaticLocal = false;
  size_t End = llvm::StringRef::npos;

  // Where the declaration says how it is linked. Blanked rather than cut out, so
  // that every byte after them keeps the offset and the line it had in the
  // original -- which is what lets an injection be placed by line and a `#line`
  // directive name the original source.
  std::vector<std::pair<size_t, size_t>> LinkageKeywords;

  for (size_t I = Start, E = Buffer.size(); I != E; ++I) {
    char C = Buffer[I];
    switch (State) {
    case Code:
      if (C == '"') {
        State = InString;
      } else if (C == '\'') {
        State = InChar;
      } else if (C == '/' && I + 1 < E && Buffer[I + 1] == '/') {
        State = InLineComment;
      } else if (C == '/' && I + 1 < E && Buffer[I + 1] == '*') {
        State = InBlockComment;
        ++I;
      } else if (C == '{') {
        SawBrace = true;
        ++Depth;
      } else if (C == '}') {
        // A close brace before any open brace means the declaration line was
        // not the start of a definition.
        if (!SawBrace)
          return Fail(BodyExtractFailure::NotFound);
        if (--Depth == 0) {
          End = I + 1;
          I = E - 1;
        }
      } else if (C == ';' && !SawBrace) {
        // A declaration rather than a definition.
        return Fail(BodyExtractFailure::NotFound);
      } else if (C == '(' && !SawBrace) {
        ++ParenDepth;
      } else if (C == ')' && !SawBrace) {
        --ParenDepth;
      } else if (Depth > 0 && C == 's' && KeywordAt(Buffer, I, "static")) {
        // Only inside the body. On the declaration `static` gives the function
        // internal linkage, which says nothing about whether a copy of it would
        // behave differently -- and refusing every file-local function would
        // refuse most of the C worth patching.
        SawStaticLocal = true;
      } else if (!SawBrace && ParenDepth == 0) {
        // Outside the parameter list, where `static` is an array bound's rather
        // than the definition's.
        for (llvm::StringRef Keyword : kLinkageKeywords) {
          if (!KeywordAt(Buffer, I, Keyword))
            continue;
          LinkageKeywords.emplace_back(I - Start, Keyword.size());
          I += Keyword.size() - 1;
          break;
        }
      }
      break;
    case InString:
      if (C == '\\')
        ++I;
      else if (C == '"')
        State = Code;
      break;
    case InChar:
      if (C == '\\')
        ++I;
      else if (C == '\'')
        State = Code;
      break;
    case InLineComment:
      if (C == '\n')
        State = Code;
      break;
    case InBlockComment:
      if (C == '*' && I + 1 < E && Buffer[I + 1] == '/') {
        ++I;
        State = Code;
      }
      break;
    }
  }

  if (!SawBrace)
    return Fail(BodyExtractFailure::NotFound);
  if (End == llvm::StringRef::npos)
    return Fail(BodyExtractFailure::Unbalanced);
  if (SawStaticLocal)
    return Fail(BodyExtractFailure::StaticLocal);

  FunctionBodyText Body;
  Body.Text = Buffer.substr(Start, End - Start).str();
  Body.FirstLine = FirstLine;

  for (const auto &[Offset, Length] : LinkageKeywords)
    if (Offset + Length <= Body.Text.size())
      Body.Text.replace(Offset, Length, Length, ' ');

  Body.LineStarts.push_back(0);
  for (size_t I = 0, E = Body.Text.size(); I != E; ++I)
    if (Body.Text[I] == '\n' && I + 1 < E)
      Body.LineStarts.push_back(I + 1);

  return Body;
}
