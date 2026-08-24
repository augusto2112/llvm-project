//===- ObservationPlan.h --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_PROTOCOL_MCP_OBSERVATIONPLAN_H
#define LLDB_SOURCE_PLUGINS_PROTOCOL_MCP_OBSERVATIONPLAN_H

#include "lldb/lldb-forward.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lldb_private::mcp {

/// Which hits of an observation reach the event stream. Reducing the stream
/// never loses counts: aggregation runs over every hit regardless of the mode.
enum class EmitMode {
  EveryHit,
  OnChange,
  FirstAndLast,
};

/// One tracepoint: where to observe, what to read there, and how much of the
/// resulting hit stream to keep.
struct Observation {
  /// Identifies this observation in the report and in another observation's
  /// \ref EnabledAfter. Defaults to \ref At, so it is only worth setting when
  /// one function is observed more than once.
  std::string Label;

  /// What to observe: either a function name, or a source location written as
  /// `file.cpp:1189`. An address is rejected because it cannot be carried from
  /// one run into the next. A trailing `:<digits>` is what distinguishes the
  /// two forms, so a qualified name like `Foo::bar` remains a function name.
  std::string At;

  /// The line within \ref At when \ref At named a source file, and unset when
  /// it named a function.
  std::optional<uint32_t> AtLine;

  /// Whether state is read as the function returns rather than as it is
  /// entered. Both resolve to the same location, the function's entry.
  bool OnReturn = false;

  /// Expressions to read at each hit. Empty is a bare tracepoint recording
  /// only hit counts, which already answers whether the code runs at all.
  std::vector<std::string> Capture;

  /// A condition evaluated at each hit. A hit whose condition is false still
  /// counts as a hit, but is neither captured nor emitted.
  std::optional<std::string> WhenExpr;

  /// Restricts the tracepoint to hits reached from this function.
  std::optional<std::string> CalledFrom;

  /// Holds this observation disabled until the observation carrying this label
  /// has been hit.
  std::optional<std::string> EnabledAfter;

  /// Hits to ignore before the tracepoint starts recording.
  uint32_t SkipFirst = 0;

  /// Records a single hit, counting from one.
  std::optional<uint32_t> OnlyHit;

  EmitMode Emit = EmitMode::EveryHit;

  /// Frames of backtrace to record per emitted event.
  uint32_t Backtrace = 0;

  /// Levels of children to expand in each captured value.
  uint32_t Depth = 2;
};

struct ObservationPlan {
  std::string Program;
  std::vector<std::string> Args;
  llvm::StringMap<std::string> Env;
  std::optional<std::string> Cwd;

  /// A path whose contents are fed to the inferior's standard input.
  std::optional<std::string> Stdin;

  /// Whether the inferior's own standard output and error are recorded. A
  /// program's last line of output is often the only evidence of how far it
  /// got, so this is on unless it is turned off.
  bool CaptureInferiorOutput = true;

  /// Wall-clock ceiling on the whole run. A run that never terminates is a
  /// result rather than a failure, so there is always a limit.
  uint32_t TimeoutSeconds = 30;

  /// Gives up after this long with no tracepoint in the plan being hit at all.
  /// Measured over hits rather than over emitted events, because an emission
  /// mode that keeps one event in a thousand is not the program stalling.
  /// Absent leaves the check disarmed, because a plan whose triggers only fire
  /// near the end of a run is legitimate and would otherwise be cut short.
  std::optional<uint32_t> NoProgressSeconds;

  /// An empty list is a legal plan: it runs the program and reports how it
  /// ended, which is crash triage.
  std::vector<Observation> Observations;
};

/// Parses and validates a plan. A field the schema does not define is an error
/// rather than a default: a silently ignored field is indistinguishable in the
/// result from one that was honoured, which leaves the caller believing it
/// observed something it did not. Errors name the offending field and say what
/// to write instead.
llvm::Expected<ObservationPlan>
ParseObservationPlan(const llvm::json::Value &Plan);

llvm::StringRef ToString(EmitMode Mode);

/// What resolving one observation's location produced.
struct LocationResolution {
  /// Breakpoint locations the name matched. Zero means the observation cannot
  /// fire; it is reported explicitly because otherwise it is indistinguishable
  /// from an observation whose code simply never ran.
  uint32_t ResolvedLocations = 0;

  /// Why nothing matched, and the nearest names that did exist. Set only when
  /// \ref ResolvedLocations is zero.
  std::optional<std::string> Error;

  /// The breakpoint created for the observation, set whether or not the name
  /// resolved. An unresolved breakpoint is kept rather than removed, since a
  /// name in a library that has not been loaded yet resolves at load time.
  lldb::BreakpointSP Breakpoint;
};

/// Creates one breakpoint per observation and reports what each name matched.
/// The result is parallel to \p Plan.Observations.
///
/// Only \ref Observation::At is resolved here; the gating locations named by
/// \ref Observation::CalledFrom and \ref Observation::EnabledAfter belong to
/// whoever installs the callbacks. Names are matched against the modules
/// loaded at the time of the call, so a plan resolved before launch sees only
/// the main executable.
std::vector<LocationResolution>
ResolveObservationLocations(ObservationPlan &Plan, Target &Tgt);

} // namespace lldb_private::mcp

#endif
