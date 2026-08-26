//===-- FunctionPatch.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_TARGET_FUNCTIONPATCH_H
#define LLDB_TARGET_FUNCTIONPATCH_H

#include "lldb/Breakpoint/BreakpointOptions.h"
#include "lldb/Target/FunctionBodySource.h"
#include "lldb/Target/PatchControlBlock.h"
#include "lldb/Target/PatchSourceBuilder.h"
#include "lldb/lldb-forward.h"
#include "lldb/lldb-types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/Error.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lldb_private {

class Target;

/// Why a function could not be patched. Every value falls back to evaluating
/// the expression at a stop, so none of these ends a run; each exists so the
/// fallback can say what happened rather than being silent.
enum class PatchFailure {
  NotArm64,
  NoProcess,
  InferiorAccessFailed,
  NoSourceFile,
  SourceNewerThanBinary,
  BodyNotFound,
  StaticLocal,
  EntryTooSmall,
  ThreadInPatchRange,
  BreakpointInPatchRange,
  CompileFailed,
  CaptureNotScalar,
  Unsupported,
};

llvm::StringRef ToString(PatchFailure Reason);

/// Whether \p Source was written enough later than \p Binary to mean the source
/// on disk is not the source the binary was built from.
///
/// Shared with the reporting that describes the same skew in prose, so that the
/// threshold has one home. A build writes the binary after the sources it read,
/// so a small positive skew is the ordinary outcome of building and says
/// nothing. Either time arriving as the epoch means it could not be read, which
/// is a comparison that cannot be made rather than a finding.
bool SourceSkewExceedsNoise(llvm::sys::TimePoint<> Source,
                            llvm::sys::TimePoint<> Binary);

/// One injection to install, in the caller's terms.
struct PatchRequest {
  /// The entry of the function to patch. The patch always goes at the entry,
  /// whatever line the injection lands on, because the whole function is
  /// recompiled.
  lldb::addr_t FunctionEntry = LLDB_INVALID_ADDRESS;

  uint32_t Line = 0;
  std::optional<std::string> Condition;
  std::vector<std::string> Captures;
  uint32_t SkipFirst = 0;
  std::optional<uint32_t> OnlyHit;
  bool Gated = false;
  bool WantStop = true;

  /// Runs when the site's trap fires, under the ordinary breakpoint callback
  /// contract.
  BreakpointHitCallback OnTrap = nullptr;
  void *Baton = nullptr;
};

/// Patches functions so that a tracepoint's own work happens inside the
/// process, and keeps track of what has been patched.
///
/// One per target. Keyed on each patched function's entry, holding that
/// function's original source text and the injections live in it, so that
/// installing another one recompiles from the original rather than patching a
/// patch.
class FunctionPatchManager {
public:
  explicit FunctionPatchManager(Target &Tgt);
  ~FunctionPatchManager();

  FunctionPatchManager(const FunctionPatchManager &) = delete;
  FunctionPatchManager &operator=(const FunctionPatchManager &) = delete;

  /// Installs \p Request, recompiling its function with every injection already
  /// live in it. Returns the new site's id.
  llvm::Expected<uint32_t> Install(const PatchRequest &Request);

  /// Drops one injection and recompiles what remains.
  llvm::Error Remove(uint32_t SiteID);

  /// Opens or closes a gated site.
  void SetGate(uint32_t SiteID, bool Open);

  /// Whether the function entered at \p Entry has been redirected.
  bool IsPatched(lldb::addr_t Entry) const;

private:
  struct PatchedFunction;

  /// Allocates the shared ring block, once, on the first install.
  llvm::Error EnsureRingBlock();

  /// Allocates one site's slot, from a pool page.
  llvm::Expected<lldb::addr_t> AllocateSiteSlot();

  /// Compiles \p Fn's current injection set and points its trampoline at the
  /// result.
  llvm::Error Recompile(PatchedFunction &Fn);

  Target &m_target;
  lldb::addr_t m_ring_address = LLDB_INVALID_ADDRESS;
  lldb::addr_t m_slot_pool = LLDB_INVALID_ADDRESS;
  size_t m_slots_used_in_page = 0;
  uint32_t m_next_site_id = 1;
  llvm::DenseMap<lldb::addr_t, std::unique_ptr<PatchedFunction>> m_functions;
  llvm::DenseMap<uint32_t, lldb::addr_t> m_site_to_function;
};

} // namespace lldb_private

#endif // LLDB_TARGET_FUNCTIONPATCH_H
