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
///   * per observation: locations resolved, hits, events emitted, resolution error
///   * per observation and capture: the rendered summary
///   * per observation: the first hit whose captures disagreed, and what each run
///     saw there
///
/// Everything compared that matched is reported by name alone under `agreed`,
/// which is what makes "did my change affect anything else" a line rather than a
/// diffing exercise.
llvm::json::Value CompareRuns(llvm::ArrayRef<ComparedRun> Runs);

/// Values a differing capture summary reports per run before it is truncated. A
/// summary is already bounded, but two of them side by side in a document that
/// exists to be short is a different budget.
constexpr unsigned MaxComparedSummaryChars = 400;

} // namespace lldb_private::mcp

#endif
