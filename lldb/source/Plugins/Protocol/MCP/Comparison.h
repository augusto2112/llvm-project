//===- Comparison.h -------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_PROTOCOL_MCP_COMPARISON_H
#define LLDB_SOURCE_PLUGINS_PROTOCOL_MCP_COMPARISON_H

#include "ObservationEngine.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/JSON.h"
#include <optional>
#include <string>

namespace lldb_private::mcp {

/// One run of a compared plan, as the comparison reads it.
struct ComparedRun {
  std::string Label;

  /// Unset when the run could not be made at all -- a program that is not there,
  /// a target that could not be created. A run that failed is still a row in the
  /// comparison, because a fix that stops the program from starting is a
  /// difference and not an absence of one.
  std::optional<ObservationResult> Result;

  /// Why the run could not be made, when it could not.
  std::string Error;
};

/// What differed between runs of one plan, and the names of what did not.
///
/// The whole value is in not returning two reports. A caller that gets both has
/// to diff them itself: on a cost curve that grows with the square of the turns,
/// two documents in the context inflate every later turn, and the diffing is set
/// arithmetic over histograms done by inference. Both event streams are on this
/// side of the call, so the comparison is exact and costs one document.
///
/// What is compared is curated rather than structural. A structural diff of two
/// results would report the artifact paths, the elapsed times and every address
/// as differences, which is true and useless. These are compared:
///
///   * how each run ended, and where
///   * what each run wrote on stdout and on stderr
///   * per observation: locations resolved, hits, events emitted, resolution error
///   * per observation and capture, as `<label>.<expr>`: what the capture was
///     observed to hold, and under `<label>.<expr>.capture` how it resolved where
///     that was not the way nearly every capture resolves
///   * per observation: the first hit whose captures disagreed, and what each run
///     saw there
///
/// Captures that could not be read and notes no observation owns are reported once
/// each at top level, deduplicated across the runs and labelled only where the runs
/// disagree about them: two runs failing at the same name is one fault in the
/// request and not a difference between them.
///
/// Everything compared that matched is reported by name alone under `agreed`,
/// which is what makes "did my change affect anything else" a line rather than a
/// diffing exercise.
llvm::json::Value CompareRuns(llvm::ArrayRef<ComparedRun> Runs);

/// Characters of text a compared value may show per run.
///
/// Two summaries side by side used to be cut to this length each, which sliced a
/// serialized document mid-token and handed back something no caller could parse.
/// Summaries are shortened structurally instead -- to the values the runs disagree
/// on -- so this now bounds only the strings a value holds, where a single captured
/// string is genuinely long.
constexpr unsigned MaxComparedSummaryChars = 400;

/// Characters of one line of a program's output shown per run. A stream is bounded
/// at 8 kB and two of those would be most of a response; the line the runs part
/// company on is what a caller reads, and for a miscompile it is the answer.
constexpr unsigned MaxComparedOutputChars = 200;

} // namespace lldb_private::mcp

#endif
