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
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 ToString(Reason));
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

/// Whether the keyword `static` begins at \p I in \p Text.
///
/// A textual check, which is what keeps this callable before anything is
/// compiled. It over-reports in one direction only: a `static` a macro would
/// have removed still refuses the patch, and refusing is the safe answer.
bool StaticKeywordAt(llvm::StringRef Text, size_t I) {
  if (!Text.substr(I).starts_with("static"))
    return false;
  const bool StartsWord =
      I == 0 || !(llvm::isAlnum(Text[I - 1]) || Text[I - 1] == '_');
  const size_t After = I + 6;
  const bool EndsWord =
      After >= Text.size() || !(llvm::isAlnum(Text[After]) || Text[After] == '_');
  return StartsWord && EndsWord;
}

} // namespace

llvm::Expected<FunctionBodyText>
lldb_private::ExtractFunctionBody(llvm::StringRef Buffer, uint32_t DeclLine) {
  const size_t Start = OffsetOfLine(Buffer, DeclLine);
  if (Start == llvm::StringRef::npos)
    return Fail(BodyExtractFailure::NotFound);

  enum { Code, InString, InChar, InLineComment, InBlockComment } State = Code;
  int Depth = 0;
  bool SawBrace = false;
  bool SawStaticLocal = false;
  size_t End = llvm::StringRef::npos;

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
      } else if (Depth > 0 && C == 's' && StaticKeywordAt(Buffer, I)) {
        // Only inside the body. On the declaration `static` gives the function
        // internal linkage, which says nothing about whether a copy of it would
        // behave differently -- and refusing every file-local function would
        // refuse most of the C worth patching.
        SawStaticLocal = true;
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
  Body.FirstLine = DeclLine;

  Body.LineStarts.push_back(0);
  for (size_t I = 0, E = Body.Text.size(); I != E; ++I)
    if (Body.Text[I] == '\n' && I + 1 < E)
      Body.LineStarts.push_back(I + 1);

  return Body;
}
