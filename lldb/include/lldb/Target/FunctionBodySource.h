//===-- FunctionBodySource.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_TARGET_FUNCTIONBODYSOURCE_H
#define LLDB_TARGET_FUNCTIONBODYSOURCE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <string>
#include <vector>

namespace lldb_private {

/// Why a function's text could not be taken from its source.
enum class BodyExtractFailure {
  /// The declaration line is past the buffer, or no brace follows it.
  NotFound,
  /// The braces never balanced before the buffer ended.
  Unbalanced,
  /// The body declares storage that outlives a call. A recompiled copy would
  /// get storage of its own, so the two would diverge on the first call.
  StaticLocal,
};

llvm::StringRef ToString(BodyExtractFailure Reason);

/// Carries which of the extraction's refusals applied, so a caller can map it
/// onto its own vocabulary without matching on the message text.
class BodyExtractError : public llvm::ErrorInfo<BodyExtractError> {
public:
  static char ID;
  explicit BodyExtractError(BodyExtractFailure Reason) : m_reason(Reason) {}
  BodyExtractFailure reason() const { return m_reason; }
  void log(llvm::raw_ostream &OS) const override { OS << ToString(m_reason); }
  std::error_code convertToErrorCode() const override {
    return llvm::inconvertibleErrorCode();
  }

private:
  BodyExtractFailure m_reason;
};

/// A function's source text, and enough of an index to write into it by line.
struct FunctionBodyText {
  /// The declaration through its matching close brace, verbatim. Verbatim is
  /// the point: the text is recompiled, so anything reformatted here is a
  /// difference between the copy and the original.
  std::string Text;

  /// The line of the original file that \ref Text starts on.
  uint32_t FirstLine = 0;

  /// One offset into \ref Text per line of it, so that line
  /// `FirstLine + N` begins at `LineStarts[N]`. Always starts with 0.
  std::vector<size_t> LineStarts;
};

/// Takes the function declared at \p DeclLine out of \p Buffer.
///
/// The opening brace is found by scanning forward from \p DeclLine rather than
/// by parsing, because a declaration wrapped across lines is ordinary and a
/// parser here would have to agree with the compiler about far more than
/// braces. From the brace the scan counts depth, skipping string and character
/// literals and both comment forms, since a brace inside any of those closes
/// nothing.
llvm::Expected<FunctionBodyText> ExtractFunctionBody(llvm::StringRef Buffer,
                                                     uint32_t DeclLine);

} // namespace lldb_private

#endif // LLDB_TARGET_FUNCTIONBODYSOURCE_H
