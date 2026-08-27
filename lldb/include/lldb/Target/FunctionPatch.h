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
#include "lldb/Symbol/CompilerType.h"
#include "lldb/Target/FunctionBodySource.h"
#include "lldb/Target/PatchControlBlock.h"
#include "lldb/Target/PatchSourceBuilder.h"
#include "lldb/lldb-forward.h"
#include "lldb/lldb-types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/Error.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lldb_private {

class Function;
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
  BreakpointInRedirectedBody,
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

/// The bytes of a recorded capture, at the width its type says it has.
///
/// The inferior copies a value into a fixed eight-byte field, so a narrower
/// scalar arrives zero-extended. Reading all eight bytes back would report the
/// padding beside a `char` as part of its value, and a byte order that differed
/// from the inferior's would reorder every multi-byte scalar.
llvm::SmallVector<uint8_t, 8> CaptureValueBytes(uint64_t Value, size_t ByteSize,
                                                lldb::ByteOrder Order);

/// One value the injected code recorded, as a value the rest of lldb can
/// format.
struct CapturedValue {
  uint32_t SiteID = 0;

  /// Which of the site's captures this is, by position in its capture list.
  uint32_t Capture = 0;

  /// The hit that recorded it, as the site's own counter numbered that hit.
  ///
  /// What a caller joins one hit's several captures by. Two threads inside one
  /// site write their records interleaved, so the order values arrive in does
  /// not say which of them belong together.
  uint64_t Hit = 0;

  lldb::ValueObjectSP Value;
};

/// What one site's slot in the inferior says about it.
struct SiteCounters {
  uint64_t Hits = 0;
  uint64_t CondTrue = 0;
};

/// What the injected code has recorded and the debugger has read back.
struct PatchDrainResult {
  std::vector<CapturedValue> Values;

  llvm::DenseMap<uint32_t, SiteCounters> Counters;

  /// Records that did not reach the caller, because the ring overwrote them
  /// before they were read. Reported rather than absorbed: a short list that
  /// looks complete is worse than one that says what is missing.
  uint64_t Lost = 0;
};

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

  /// The breakpoint whose hits \ref OnTrap will report, when there is one.
  ///
  /// A redirected entry means the original body never runs again, so a
  /// breakpoint located in it is ordinarily a reason to refuse the patch: its
  /// trap would stop firing and nothing would say so. Not this one. Its hits
  /// are what the injection exists to deliver, and it is the caller's own --
  /// naming it here is what keeps a caller from being refused on account of the
  /// very breakpoint it is installing the patch for.
  lldb::break_id_t HitsCarriedBy = LLDB_INVALID_BREAK_ID;

  /// What \ref OnTrap is handed. A baton rather than a bare pointer because the
  /// callback outlives the call that installed it by as long as the patch runs,
  /// so whatever it needs has to be owned by something with that lifetime.
  lldb::BatonSP Baton;
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

  /// What became of one request in a batch.
  ///
  /// Requests landing in the same function are compiled together, so they
  /// succeed or fail together and every failure of a group carries the same
  /// reason: what the compile could not do it could not do for any of them.
  struct InstallOutcome {
    std::optional<uint32_t> SiteID;

    /// Why, set exactly when \ref SiteID is not.
    std::string Refusal;
  };

  /// Installs every one of \p Requests, recompiling each function they land in
  /// once for all the injections it gains, and answers for each in turn.
  ///
  /// One compile per function rather than one per injection. A compile runs clang
  /// over the whole function body and JITs the result into the program, and each
  /// one retires a copy that is kept for the life of the target and leaves a
  /// location behind in every breakpoint over those lines -- so a plan with
  /// several tracepoints in one function should pay for one.
  std::vector<InstallOutcome> Install(llvm::ArrayRef<PatchRequest> Requests);

  /// Installs \p Request, recompiling its function with every injection already
  /// live in it. Returns the new site's id.
  llvm::Expected<uint32_t> Install(const PatchRequest &Request);

  /// Drops one injection and recompiles what remains.
  ///
  /// The recompile happens first and nothing is forgotten until it has
  /// succeeded, so a failure leaves the site exactly as it was: still installed,
  /// still able to stop, and still testing what it was compiled with. A caller
  /// that has been refused here has an out-of-date injection rather than none.
  llvm::Error Remove(uint32_t SiteID);

  /// Reads whatever the injected code has recorded since the last read, and
  /// holds it until \ref Drain hands it to a caller.
  ///
  /// Separate from \ref Drain because the reads that keep the ring from
  /// overwriting itself are the debugger's own errand: they happen at traps and
  /// at stops nobody asked a question at, where there is no caller to hand a
  /// value to. Reading without taking is what lets those happen as often as
  /// they need to.
  ///
  /// Callable only while the process is held still, since inferior memory
  /// cannot be read while it runs.
  llvm::Error CollectRecords();

  /// Takes everything recorded since the last call, as typed values.
  ///
  /// Each value carries the type its capture's local had in the patched copy's
  /// debug info, so the formatters that print a variable print this too.
  ///
  /// Reads the inferior first when it can, so a caller that only ever calls
  /// this still sees every record; a call that finds nothing new costs a memory
  /// read.
  llvm::Expected<PatchDrainResult> Drain();

  /// Why the values recorded after the last stop of a run may never arrive, or
  /// empty when nothing stands in their way.
  ///
  /// A run that never fills the ring never raises a drain trap, so its records
  /// and its hit counts are read at the stop the exit path takes. When no exit
  /// symbol could be found there is no such stop, and a caller told nothing
  /// would read the missing tail as a run that recorded nothing.
  llvm::StringRef GetTailDrainRefusal() const { return m_tail_drain_refusal; }

  /// Why a capture of \p SiteID was dropped, one string per dropped capture.
  ///
  /// A capture whose type the copy's debug info says is not a scalar is dropped
  /// on its own rather than refusing the injection, so the reason for it has to
  /// be reachable on its own too.
  llvm::ArrayRef<std::string> GetDroppedCaptures(uint32_t SiteID) const;

  /// Takes what has been compiled into the program back out of it, as far as a
  /// program that is about to run unwatched allows.
  ///
  /// For a process the debugger is about to detach from. A trap the program's own
  /// code contains raises a signal with nothing there to answer it, which kills
  /// the program -- measured: an inferior detached from with a compiled-in
  /// condition still in it died of SIGTRAP on the next hit that condition held
  /// for. So every trap in every copy is written over with a `nop`, and each
  /// redirected entry is put back so that the program runs the code it was built
  /// as.
  ///
  /// Returns the functions whose entry could not be put back, which is any whose
  /// trampoline a thread is parked inside. Those go on running a copy -- one with
  /// no traps left in it, so the program survives -- and that is worth saying,
  /// since the copy is compiled without optimization and reads a control block
  /// the debugger allocated.
  llvm::Expected<std::vector<ConstString>> WithdrawFromProcess();

  ///
  /// Forgets every patch, for a process that is gone: the copies, the redirects
  /// into them, and the inferior addresses they were compiled around.
  ///
  /// Not the tag sequence. The declarations a compile leaves behind belong to
  /// the target rather than to the process, so a tag reused after a relaunch
  /// would redefine the last run's.
  void ForgetProcess();

  /// Opens or closes a gated site.
  void SetGate(uint32_t SiteID, bool Open);

  /// Whether the function entered at \p Entry has been redirected.
  bool IsPatched(lldb::addr_t Entry) const;

  /// Whether any function has been redirected at all, which is what makes asking
  /// about one worth the symbol lookup it costs.
  bool HasPatches() const { return !m_functions.empty(); }

private:
  struct PatchedFunction;

  /// One capture's type, and what it is a capture of.
  struct RecordedCapture {
    /// Read from the `__lldb_cap_*` local in the compiled copy's debug info,
    /// which is the only place the type is known: `__typeof__` is what decides
    /// it, so only the compiler can say what it was.
    CompilerType Type;

    /// The expression as the caller wrote it, which is the name a recorded
    /// value is reported under. A value labelled by its position in a capture
    /// list says nothing about what was captured.
    std::string Expression;
  };

  /// Allocates the shared ring block, once, on the first install.
  llvm::Error EnsureRingBlock();

  /// Allocates one site's slot, from a pool page.
  llvm::Expected<lldb::addr_t> AllocateSiteSlot();

  /// Compiles \p Fn's current injection set and points its trampoline at the
  /// result.
  llvm::Error Recompile(PatchedFunction &Fn);

  /// Installs the requests of \p Requests named by \p Which, all of which land
  /// in the function entered at \p Entry, with one compile between them.
  void InstallInOneFunction(lldb::addr_t Entry,
                            llvm::ArrayRef<PatchRequest> Requests,
                            llvm::ArrayRef<size_t> Which,
                            std::vector<InstallOutcome> &Outcomes);

  /// Every module describing a copy this manager has compiled, live or retired.
  std::vector<lldb::ModuleSP> CopyModules() const;

  /// Records the type the compiled copy's debug info gives every capture of \p
  /// Fn's injections, and drops from \p Fn the captures that type refuses.
  ///
  /// Returns whether anything was dropped, which means the copy just compiled
  /// records something it should not and has to be compiled again. Returns an
  /// error instead where the injection that would lose the capture does not
  /// stop: the recording is then the only way that value ever leaves the
  /// program, so dropping it would report the hit with one of the values the
  /// caller asked for silently missing.
  llvm::Expected<bool> RecordCaptureTypes(PatchedFunction &Fn, Function &Copy,
                                          llvm::StringRef Tag);

  /// Sets the internal breakpoint whose stop the tail of a run is read at.
  ///
  /// A run that takes no stop of its own never reads what its patches recorded,
  /// so without a stop on the way out neither its values nor its hit counts
  /// leave the program at all.
  void EnsureExitDrainBreakpoint();

  /// Registers the site that attributes the trap the compiled copy contains at
  /// \p Trap, and gives it an internal breakpoint of its own to carry \p
  /// OnTrap. Returns that breakpoint's id.
  llvm::Expected<lldb::break_id_t>
  RegisterTrapSite(lldb::addr_t Trap, BreakpointHitCallback OnTrap,
                   const lldb::BatonSP &Baton);

  /// Makes the site carried by \p BreakID resume without reporting anything.
  void SilenceSite(lldb::break_id_t BreakID);

  Target &m_target;
  lldb::addr_t m_ring_address = LLDB_INVALID_ADDRESS;
  lldb::addr_t m_slot_pool = LLDB_INVALID_ADDRESS;
  size_t m_slots_used_in_page = 0;
  uint32_t m_next_site_id = 1;

  /// Counts compiles so each gets a \ref PatchSourceRequest::Tag of its own.
  /// A site id would also be unique, but a recompile that only drops an
  /// injection has no new site to name it after, so the tag is its own
  /// sequence rather than borrowed from one that does not always advance.
  uint32_t m_next_compile_tag = 0;

  /// The internal breakpoint the tail of a run is read at, if one could be set.
  lldb::break_id_t m_exit_drain = LLDB_INVALID_BREAK_ID;

  std::string m_tail_drain_refusal;

  /// Records read out of the ring but not yet handed to a caller. Held raw
  /// rather than as values, because a read the debugger made for its own sake
  /// should cost a memcpy per record rather than a value object. Bounded, and
  /// lossy at the bound like the ring it came out of.
  std::vector<PatchRecord> m_pending;

  /// Records that will never reach a caller, since the last time one was told.
  uint64_t m_pending_lost = 0;

  /// Every site's counters as of the last read. Cached rather than read when
  /// asked for, because a run's final counters are only readable while the
  /// process holding them is still there.
  llvm::DenseMap<uint32_t, SiteCounters> m_counters;

  /// Where each live site's counters are, which is what makes them readable.
  llvm::DenseMap<uint32_t, lldb::addr_t> m_site_slots;

  /// Each site's captures, in the order the injected code numbers them.
  llvm::DenseMap<uint32_t, std::vector<RecordedCapture>> m_captures;

  llvm::DenseMap<uint32_t, std::vector<std::string>> m_dropped_captures;

  llvm::DenseMap<lldb::addr_t, std::unique_ptr<PatchedFunction>> m_functions;
  llvm::DenseMap<uint32_t, lldb::addr_t> m_site_to_function;
};

} // namespace lldb_private

#endif // LLDB_TARGET_FUNCTIONPATCH_H
