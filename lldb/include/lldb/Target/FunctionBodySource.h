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
  /// The declaration through its matching close brace, byte for byte, save that
  /// the keywords saying how the definition is linked are blanked.
  ///
  /// Byte for byte is the point: the text is recompiled, so anything reformatted
  /// here is a difference between the copy and the original. The blanking is
  /// written over the keyword rather than cut out for the same reason -- every
  /// byte after it keeps the offset and the line it had, which is what lets an
  /// injection be placed by line.
  ///
  /// `static` and `inline` are what is blanked. Neither says anything about what
  /// the body does, and each of them kept makes the copy disagree with the
  /// declaration the expression parser derives from the program's own debug
  /// info, which is a definition the compiler refuses.
  std::string Text;

  /// The line of the original file that \ref Text starts on.
  ///
  /// Not necessarily the line debug info gave the declaration: a signature
  /// broken across lines is declared from its return type onwards, which is
  /// above the line its name is on.
  uint32_t FirstLine = 0;

  /// One offset into \ref Text per line of it, so that line
  /// `FirstLine + N` begins at `LineStarts[N]`. Always starts with 0.
  std::vector<size_t> LineStarts;
};

/// Takes the function whose name is on \p DeclLine out of \p Buffer.
///
/// The text starts where the declaration starts, which is not always \p
/// DeclLine: `DW_AT_decl_line` names the line the function's name is on, and a
/// signature broken across lines has its return type and storage class above
/// that. So the lines immediately above are taken too, for as long as each can
/// only be leading into the next.
///
/// The opening brace is found by scanning forward rather than by parsing,
/// because a declaration wrapped across lines is ordinary and a parser here
/// would have to agree with the compiler about far more than braces. From the
/// brace the scan counts depth, skipping string and character literals and both
/// comment forms, since a brace inside any of those closes nothing.
llvm::Expected<FunctionBodyText> ExtractFunctionBody(llvm::StringRef Buffer,
                                                     uint32_t DeclLine);

} // namespace lldb_private

#endif // LLDB_TARGET_FUNCTIONBODYSOURCE_H
