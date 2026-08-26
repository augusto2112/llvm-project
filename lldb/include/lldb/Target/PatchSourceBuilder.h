//===-- PatchSourceBuilder.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_TARGET_PATCHSOURCEBUILDER_H
#define LLDB_TARGET_PATCHSOURCEBUILDER_H

#include "lldb/Target/FunctionBodySource.h"
#include "lldb/lldb-types.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lldb_private {

/// One place in a function where code is injected, and what it does there.
struct PatchInjection {
  uint32_t SiteID = 0;

  /// The line of the original file this attaches to. The injected code is
  /// emitted before that line and claims the same line number, so a stop just
  /// past the trap reports the line the caller asked about.
  uint32_t Line = 0;

  /// Where this site's own counters live in the inferior, baked in as a
  /// literal.
  lldb::addr_t SlotAddress = 0;

  /// Trap when this holds. Absent means unconditional, which is what a site
  /// that only records wants.
  std::optional<std::string> Condition;

  /// Expressions whose values are recorded. Each must be a scalar of eight
  /// bytes or fewer, which is checked against the compiled copy's debug info
  /// rather than here.
  std::vector<std::string> Captures;

  uint32_t SkipFirst = 0;
  std::optional<uint32_t> OnlyHit;

  /// Whether the site consults its gate byte before doing anything.
  bool Gated = false;

  /// Whether a hit that passes every guard should stop the process. False for
  /// a site that only records, where stopping would cost exactly what the
  /// recording avoids.
  bool WantStop = true;
};

/// Everything needed to write one patched function's source.
struct PatchSourceRequest {
  FunctionBodyText Body;

  /// The original file, named in every `#line` so the copy's debug info points
  /// at the source the caller is reading.
  std::string SourcePath;

  lldb::addr_t RingAddress = 0;

  /// Records the ring holds. A power of two, emitted as a mask.
  uint64_t RingCapacity = 0;

  std::vector<PatchInjection> Injections;
};

/// The local that holds capture \p Capture of site \p SiteID.
///
/// Its type in the compiled copy's debug info is how the capture's type is
/// recovered, so the caller reading that debug info and the builder emitting
/// the local have to agree on this spelling.
std::string CaptureLocalName(uint32_t SiteID, uint32_t Capture);

/// Writes the top-level C source for one patched function.
///
/// An injection whose line falls outside the body is dropped rather than
/// clamped: the declaration line and the closing brace are not statements, so
/// there is nowhere valid to put it, and moving it somewhere else would report
/// hits for a line the caller did not name.
std::string BuildPatchSource(const PatchSourceRequest &Request);

} // namespace lldb_private

#endif // LLDB_TARGET_PATCHSOURCEBUILDER_H
