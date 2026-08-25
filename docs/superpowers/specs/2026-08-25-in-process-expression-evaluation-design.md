# In-process expression evaluation (arm64 prototype)

Date: 2026-08-25
Status: design approved, not yet implemented
Scope: arm64 only. Darwin is the only tested platform.

## Problem

lldb-mcp's `observe` tool evaluates expressions at tracepoints in two roles: a
`when` condition that decides whether a hit is recorded, and `capture`
expressions whose values go into the report. Both run under the same pattern:
stop the process, context-switch to the debug server, evaluate, continue.

The stop is the cost. It is paid per hit, whether or not the condition holds,
and a condition that is false a million times costs a million stops. Since
expression evaluation is both how lldb-mcp decides where to stop and largely how
it gathers information, this cost sets the ceiling on what the tool can observe.

This design removes the stop from the hot path by compiling the expression into
the program.

## Approach

Given an expression at a location:

1. Read the enclosing function's textual body from source.
2. Inject the expression into that text at the right line.
3. JIT-compile the result into the inferior as top-level code.
4. Overwrite the original function's entry with a 16-byte trampoline to the copy.

A false condition then costs two instructions and no stop. A true condition
raises a trap the debugger attributes as an ordinary breakpoint hit.

### Why the patch point is the function entry

Recompiling the whole function means the injection point inside the body is
independent of the patch point in the machine code. The patch is always at the
function's entry, no matter where the injection lands.

That makes the trampoline ABI-trivial. At a function entry, `x0`-`x7` hold
arguments, `x8` the indirect result pointer, `lr` the return address, and `sp`
the caller's stack — exactly what the recompiled copy expects, because it has
the same signature. `x16`/`x17` (IP0/IP1) are architecturally scratch at a call
boundary, reserved for linker veneers, which is precisely this use. So the
trampoline saves nothing and restores nothing:

```
ldr x16, [pc, #8]     ; 0x58000050
br  x16               ; 0xD61F0200
.quad <patched copy>   ; 8 bytes, position-independent, full 64-bit range
```

The alternative — patching instructions at the breakpoint's own address —
requires saving every caller-saved register, limits the condition to variables
whose DWARF locations are live at that PC, and needs an arm64 displaced-
instruction relocator (`adr`, `adrp`, `b.cond`, `cbz`, `tbz`, `ldr`-literal all
need rewriting). That relocator is the whole project. It was rejected.

Recompiling from source also means the injected expression sees the function's
locals as real locals of the recompiled body. No materialization, no
dematerialization, no variable marshalling.

### Verified before committing to this design

Measured against a real arm64 process with `build/bin/lldb` at
`484746de51e7`:

| Claim | Result |
| --- | --- |
| `eExecutionPolicyTopLevel` compiles a whole function body | Resolved `struct Point` from DWARF and linked a call to `helper`, with no headers supplied |
| The JIT'd copy is addressable and runs | Executed with real arguments |
| A 16-byte entry trampoline redirects all calls | Arguments intact; unwind intact (caller frame correctly identified) |
| An in-process condition plus `__builtin_debugtrap()` traps | Yes |
| `#line` redirects clang's debug info to the original source | Whole patched body attributed to the original file and lines |
| Expression JIT modules register as target images | **No.** See "The JIT module registration gap" |

The trap's PC convention was measured, and it decides the whole trap design:

| Trap | PC on stop |
| --- | --- |
| `brk #0` (`0xD4200000`, LLDB's own opcode) | **at** the trap |
| `brk #0xf000` (`0xD43E0000`, `__builtin_debugtrap()`) | **past** the trap (+4) |

`__builtin_debugtrap()` is therefore the right trap: the kernel has already
stepped over it, so resuming needs no PC arithmetic and no step-over plan.

## Architecture

### New components

All of these except `FunctionPatch` are pure — no process required, so they are
unit-testable directly.

| File | Responsibility |
| --- | --- |
| `Target/FunctionBodySource.{h,cpp}` | Given a `Function` and a `SourceManager`, return its textual extent: the start of the declaration through the matching `}`, brace-balanced with string, character, line-comment and block-comment awareness, plus a line-to-offset index for placing injections. |
| `Target/PatchSourceBuilder.{h,cpp}` | Assemble the generated top-level source from the body text and the injection list. Owns the `#line` discipline and the baked-in control block addresses. Returns a string. |
| `Target/PatchControlBlock.{h,cpp}` | Layout of the inferior-side control block, record encode and decode, and the ring drain producing records plus a lost count. |
| `Target/EntryTrampoline.{h,cpp}` | `EncodeEntryTrampoline(addr_t) -> array<uint8_t, 16>`. Pure byte encoding only; whether a function *can* be patched — 16 bytes inside its bounds, no thread's PC in the range, no breakpoint site in it — needs live process state and belongs to the manager. |
| `Target/FunctionPatch.{h,cpp}` | Orchestration, and the only stateful part. `FunctionPatchManager`, one per `Target`, keyed by original entry address. Holds each patched function's original body text, its live injections, the current JIT'd copy, and the trampoline's saved bytes. |

### The facility's interface

```c++
/// One place in one function where code is injected, and what to do there.
struct Injection {
  uint32_t Line;                          ///< Line in the original source.
  std::optional<std::string> Condition;   ///< Trap when this holds.
  std::vector<std::string> Captures;      ///< Recorded per qualifying hit.
  uint32_t SkipFirst = 0;
  std::optional<uint32_t> OnlyHit;
  bool Gated = false;                     ///< Consult the gate byte first.
  bool WantStop = true;                   ///< False for capture-only sites.
  BreakpointHitCallback OnTrap;            ///< Runs when the stop trap fires.
  void *Baton = nullptr;
};

/// Why a function could not be patched. Every value falls back to
/// stop-and-evaluate, and each is nameable in a report.
enum class PatchFailure {
  NotArm64, NoProcess, NoSourceFile, SourceNewerThanBinary, BodyNotFound,
  StaticLocal, EntryTooSmall, ThreadInPatchRange, BreakpointInPatchRange,
  CompileFailed, CaptureNotScalar, Unsupported,
};

class FunctionPatchManager {
public:
  /// Installs \p Inj in the function entered at \p Entry, recompiling that
  /// function from its original source with every injection this manager
  /// already holds for it. Returns the new site's id.
  llvm::Expected<uint32_t> Install(const Address &Entry, Injection Inj);

  /// Drops one injection and recompiles what remains. The copy carrying it is
  /// retired rather than freed.
  llvm::Error Remove(uint32_t SiteID);

  /// Reads and clears the record ring, and reads the per-site counters.
  /// Callable only while stopped.
  llvm::Expected<DrainResult> Drain();

  /// Opens or closes a gated site, from a gate breakpoint's callback.
  void SetGate(uint32_t SiteID, bool Open);
};

/// What one drain produced. \ref Lost is how many records the ring overwrote
/// before they were read, which is reported rather than silently absorbed.
struct DrainResult {
  std::vector<Record> Records;
  llvm::DenseMap<uint32_t, SiteCounters> Counters;
  uint64_t Lost = 0;
};
```

Each capture's `CompilerType` is read out of the patched module's debug info at
install time, from the `__lldb_cap_K_I` local that holds it, and cached against
the site so a drained record can be turned back into a typed value.

### Core changes

- `Breakpoint/BreakpointSite.h` — add a `eProgramTrap` type: *the trap is
  already in the program's own code*. Follows the existing `eExternal` type for
  read-shadow exclusion; enable and disable are no-ops, with no `Z0` packet.
- `Target/Process.{h,cpp}` — a `CreateBreakpointSite(owner, addr, type)`
  overload. The existing entry point derives the site's address from the owner
  location, and this design needs a site at `trap+4` owned by a location
  elsewhere. `BreakpointSite`'s constructor already takes the address
  separately, so this is additive.
- `Target/Target.h` — `GetFunctionPatchManager()`.
- `Target/TargetProperties.td` — `target.experimental.fast-conditions`,
  defaulting to off.
- `Breakpoint/BreakpointLocation.cpp` — with the setting on, route a new
  condition through the manager. On success the location's own site comes down
  and the `eProgramTrap` site carries the hits.
- `Expression/LLVMUserExpression` — a `GetJITModule()` accessor. See below.

### Why `eProgramTrap` is required, not merely convenient

`PC == trap+4` arises two ways: the trap fired and the kernel advanced PC, or
the condition was false and the branch skipped to that address. A normal
breakpoint site at `trap+4` would write its own trap and fire in **both** cases,
which is to say on every hit — defeating the feature entirely.

A site that never writes an opcode is only ever reached through the exception the
program raised itself. The site is a label for attributing a trap the code
raised, not a mechanism for causing one. That is the whole distinction, and it is
why the type has to exist.

### The JIT module registration gap

Nothing currently registers a user expression's JIT module as a target image:
`LLVMUserExpression::m_jit_module_wp` is never assigned, and only
`FunctionCaller` appends one. This is measurable — a patched copy has no symbol
and no line entry, so a stop inside it reports no function and no source.

The manager appends the module itself, with notification. Notification is what
makes LLDB re-resolve breakpoints into the new module, which is how a
`file:line` breakpoint set after patching gains a location in the patched copy.

## The generated source

Worked example: original at `t.c` lines 4-9, condition `acc > 5` at line 7,
capturing `acc` and `p->x`.

```c
// Preamble, attributed to a synthetic file so it never shadows real lines.
struct __lldb_rec_t { unsigned int site, cap; unsigned long long val; };
struct __lldb_hdr_t { unsigned long seq, drained, capacity, high_water;
                      struct __lldb_rec_t ring[]; };
struct __lldb_site_t { unsigned char gate; unsigned long hits, cond_true; };
#define __LLDB_HDR    ((volatile struct __lldb_hdr_t  *)0x104000000UL)
#define __LLDB_SITE_K ((volatile struct __lldb_site_t *)0x104010018UL)
static void __lldb_rec(unsigned k, unsigned c, unsigned long long v) {
  unsigned long s = __atomic_fetch_add(&__LLDB_HDR->seq, 1, __ATOMIC_RELAXED);
  volatile struct __lldb_rec_t *r = &__LLDB_HDR->ring[s & (CAPACITY - 1)];
  r->site = k; r->cap = c; r->val = v;
}

#line 4 "/tmp/ipe/t.c"
int target(struct Point *p, int n) {
  int acc = 0;
  for (int i = 0; i < n; i++) {
#line 7 "/tmp/ipe/t.c"
    if (__LLDB_SITE_K->gate) {
      unsigned long __h =
          __atomic_add_fetch(&__LLDB_SITE_K->hits, 1, __ATOMIC_RELAXED);
      if (__h > SKIP) {
        if (acc > 5) {
          __atomic_add_fetch(&__LLDB_SITE_K->cond_true, 1, __ATOMIC_RELAXED);
          __typeof__(acc)  __lldb_cap_K_0 = (acc);
          __typeof__(p->x) __lldb_cap_K_1 = (p->x);
          unsigned long long __v0 = 0, __v1 = 0;
          __builtin_memcpy(&__v0, &__lldb_cap_K_0, sizeof __lldb_cap_K_0);
          __builtin_memcpy(&__v1, &__lldb_cap_K_1, sizeof __lldb_cap_K_1);
          __lldb_rec(K, 0, __v0);
          __lldb_rec(K, 1, __v1);
          __builtin_debugtrap();      /* only when this site wants a stop */
        }
      }
      if (__LLDB_HDR->seq - __LLDB_HDR->drained >= __LLDB_HDR->high_water)
        __builtin_debugtrap();        /* drain trap; engine drains and resumes */
    }
#line 7 "/tmp/ipe/t.c"
    acc += helper(p->x + i);
  }
  return acc + p->y;
}
```

Four properties this layout buys.

**The doubled `#line 7`** attributes both the injected block and the original
statement to line 7, so a stop at `PC == trap+4` reports "at line 7, about to
execute it" — as if the expression were not there. Verified against clang's
emitted line table.

**`__typeof__` plus `__builtin_memcpy` into a `u64`** means the builder needs no
type knowledge at all, and the copy is bit-exact for every scalar: integers,
floats, pointers, bools, enums. A cast to `unsigned long long` would silently
truncate a `double`; a memcpy will not.

**The `__lldb_cap_K_I` locals carry their real types in the patched module's
DWARF.** That is how the engine rebuilds a typed `ValueObjectConstResult` from
eight raw bytes, which in turn is how the existing formatters and serialization
path apply unchanged. It is also the scalar gate: after compiling, a capture
whose type is not a scalar of eight bytes or fewer is rejected and falls back.

**Hit counting matches the documented semantics.** `ObservationPlan.h` states
that a hit whose condition is false still counts as a hit, so the counter is
bumped before the condition and `cond_true` inside it. The report's existing
`hits` and `condition_true` fields keep working, read from the control block at
drain time.

Each injection yields up to two trap addresses, the stop trap and the drain
trap. A condition-only injection with no captures emits no drain trap.

### What is compiled in, and what still stops

| Observation field | Handling |
| --- | --- |
| `when` | Compiled as the branch guarding the trap |
| `capture` (scalar) | Compiled; recorded to the ring |
| `capture` (non-scalar) | Falls back |
| `skip_first`, `only_hit` | Compiled as a per-site atomic counter in the control block |
| `called_from`, `enabled_after` | Compiled as a per-site gate byte the engine flips from the gate breakpoint's callback. The gate breakpoint still stops, but that is O(gate hits), not O(tracepoint hits) |
| `on_return` | Falls back. Injecting before every `return` is textually fragile, and `on_return` wants the return value |
| `emit` modes other than `EveryHit` | Falls back |
| `backtrace` | Falls back; a backtrace needs a stop |

## Re-patching

The requirement is that changing an already-changed function composes, with both
changes in the program's text.

The manager holds the **original** body text, read once, plus the live injection
list. Installing or removing an injection regenerates the source from the
original with every live injection applied, JITs a fresh copy, and rewrites only
the trampoline's 8-byte literal. A patched copy is never patched again.

The literal is 8-byte aligned, so re-pointing is a single store.

Old copies are **retired, not freed**, because an in-flight call has frames
inside one. The retired module stays alive for the life of the target and its
trap sites stay registered, so an in-flight call keeps reporting hits into the
same control-block slots — the same observation, correctly counted. When an
injection is removed, the retired copy's sites are converted to silent-resume
handlers rather than unregistered, so a straggler cannot surface as a bare
`EXC_BREAKPOINT`. No liveness tracking is needed.

## The control block

Two allocations, both made through `Process::AllocateMemory`, read and write, not
executable.

**The ring block**, allocated once per target on first install: `seq`,
`drained`, `capacity`, `high_water`, and the record ring. Its address is baked
into every generated patch as a literal, which is what keeps it stable across
re-patches; a global defined inside a JIT module would move every time the
function was recompiled.

**Site slots**, one 24-byte slot per injection, sub-allocated from pool pages.
Each slot's own absolute address is baked into the patch that uses it, so a slot
holds `gate`, `hits` and `cond_true` for exactly one site.

Per-site state deliberately does **not** live in arrays inside the ring block.
It would work, but `hits[K]`'s offset depends on the array length, so growing the
site count would silently invalidate the offsets compiled into every patch
already in the program — making the site count a hard limit fixed at allocation.
Giving each site its own address instead means adding a site allocates a slot,
disturbs nothing, and needs no recompile. There is no site limit beyond memory,
and the ring block's layout is fixed for good.

A slot is not reclaimed when its injection is removed, because retired copies
still reference it. At 24 bytes, and a page holding 170 of them, leaking one per
install-and-remove cycle is not worth tracking liveness to avoid.

The debugger writes `drained`, `capacity`, `high_water` and each slot's `gate`.
The inferior writes `seq`, the ring records, and each slot's `hits` and
`cond_true`. Defaults: 4096 records (64 KB), `high_water` at three quarters of
capacity.

Capacity is a power of two so the writer indexes with a mask rather than a
division. It is fixed for the life of the ring block, which lets the builder bake
it into the generated source as a literal and spares the writer a load. The
header carries it as well, written once by the debugger at allocation, so the
reader and writer cannot disagree about it.

The ring is deliberately lossy. `seq - drained > capacity` says exactly how many
records were lost, and that number is reported rather than a short list being
returned as if complete.

Two reasons this is a ring and not a linear buffer. It normally never wraps —
the drain trap fires at three quarters full, so wrap-around only occurs in the
race window between the trap firing and the stop being delivered, while other
threads still run. And `ring[seq & mask]` cannot write out of bounds with no
hot-path branch, where a linear buffer needs a bounds check on every record and
can corrupt adjacent memory if the accounting is ever wrong. The wrap semantics
themselves — losing oldest rather than newest — barely matter to aggregation.

### Drain policy

The control block and everything in this section exist only for captures, so
they are reached only by lldb-mcp. A `breakpoint set -c` condition writes no
records, needs no ring, and never arms a drain trap.

Inferior memory cannot be read while the process runs, which is the constraint
the whole policy answers.

1. The drain trap fires at `seq - drained >= high_water`: one stop per roughly
   3000 records rather than one per hit.
2. Every stop the engine already makes also drains — a condition trap, an
   unrelated breakpoint, a crash, the timeout ceiling. Free.
3. The tail: a run with ten hits never fills the ring and may never stop again.
   The engine sets an internal breakpoint on the exit path (`exit`, `_exit`) and
   drains there. If that does not resolve, the undrained count is reported rather
   than dropped.

A drain trap can fire with nothing left to drain. Threads keep running until the
stop is delivered, so several may cross the high-water mark before the first
drain completes, and the trap is also outside the per-site skip and condition
checks so any open site can raise it. The handler therefore treats an empty drain
as ordinary and resumes without reporting anything.

A pipe would remove the tail problem entirely, since LLDB can read a pipe while
the process runs. It was rejected because a `write()` per record is a kernel
round trip on the hot path, which is the cost this feature exists to remove: a
ring store is about 10 ns and stays in userspace, a `write` about 1 µs. Still
far cheaper than a stop, but it undercuts the claim for capture-heavy plans.

## Failure policy

Every failure falls back to today's stop-and-evaluate path, and none is silent.
The manager returns a typed reason:

| Reason | Meaning |
| --- | --- |
| `NotArm64`, `NoProcess` | Out of scope for the prototype |
| `NoSourceFile` | The function's source is not on disk |
| `SourceNewerThanBinary` | Refused; see below |
| `BodyNotFound` | No `DW_AT_decl_line`, or unbalanced braces |
| `StaticLocal` | The body declares a `static`; the copy would get its own storage, diverging from the original's |
| `EntryTooSmall` | Fewer than 16 bytes of function to overwrite |
| `ThreadInPatchRange` | A thread is parked inside the bytes to be overwritten |
| `BreakpointInPatchRange` | A breakpoint site overlaps them |
| `CompileFailed` | Carries the clang diagnostic. The catch-all that absorbs macros, missing types, C++, Objective-C and Swift |
| `CaptureNotScalar` | Per capture, determined after compiling |
| `Unsupported` | `on_return`, or an emit mode other than `EveryHit` |

`SourceNewerThanBinary` is a refusal on principle rather than a convenience.
Recompiling edited source does not add a condition to a function; it substitutes
a different function, silently changing the program under test.
`DescribeSourceSkew` in `ObservationPlan.h` already makes exactly this
comparison and is reused.

### Report surface

One field per observation, given how carefully `ObserveSurface.cpp` accounts for
schema bytes: `"eval": "in-process"`, or `"eval": "stopped: <reason>"`.

One run-level note naming the functions that were recompiled, because the
program under test really is running an `-O0` copy of them and that belongs in
the report once rather than being left to be inferred.

A plan-level `"fast": false` forces the old path, for anyone chasing a
heisenbug who needs to remove the variable. In-process evaluation is otherwise
on by default for lldb-mcp.

## Known behavioural differences

Stated rather than papered over, because a prototype that hides these is worse
than one that admits them.

- **A patched function loses its optimized codegen.** The copy is compiled at
  `-O0`, so a hot function gets slower even as its stops disappear. Matching the
  original's `-O` level was considered and rejected: it does not reproduce the
  original codegen either, so it buys false confidence while making failures
  harder to read.
- **Semantics can drift** for the same reason — float contraction, undefined
  behaviour the optimizer exploited, and callee inlining all differ between the
  original and the copy.
- **An in-process capture runs without a timeout.** `CaptureOptions()` gives a
  capture 50 ms today, so a runaway call fails one capture. In-process there is
  no such escape and a runaway call hangs the program.
- **Inlined copies are not patched.** A `file:line` breakpoint resolves to every
  inlined instance plus the out-of-line body. Patching covers the out-of-line
  body; the other locations keep the stop-and-evaluate path, so hits are not
  lost, only slower.
- **In-flight calls are not patched.** A call already on the stack runs the old
  body to completion. This matches the stated constraint that patching happens
  from outside the function.
- **arm64e is untested.** `br x16` needs no signing, but a BTI landing-pad
  requirement at the target would fault. The prototype restricts itself to
  arm64.

## Testing

Unit tests, no process required:

- `FunctionBodySource` — brace balancing through strings, character literals,
  line comments and block comments; nested braces; `static` detection; a missing
  source file.
- `PatchSourceBuilder` — golden strings; `#line` placement; several injections in
  one function; injection ordering by line.
- `PatchControlBlock` — record encode and decode; ring wraparound; lost-count
  arithmetic.
- `EntryTrampoline` — the exact 16 bytes. clang's reference encoding for
  `ldr x16, .+8; br x16` is `58000050 d61f0200`.

API tests, gated on arm64 and Darwin, driven through `breakpoint set -c` because
it is a far lighter harness than the MCP tool for debugging a JIT problem:

1. **A condition false 100,000 times, asserting the process was never stopped
   and the run completed.** This is the test of the feature's actual purpose, so
   it is written first.
2. A condition true once: assert the stop lands on the right file and line with
   the right locals.
3. Re-patching: a second condition in the same function, with both firing.
4. Captures end to end through the MCP `observe` tool, in `TestObserve.py`.
5. One negative test per refusal that matters: a function too small to patch, a
   `static` local, a macro in the body.

## Naming the generated function

The generated function keeps the original function's name, and its address is
read from the JIT module's symbol table rather than by evaluating `&name` as an
expression.

A unique source-level name with an `asm` label redirecting the symbol was the
first choice, so that the DWARF name stayed `target` while the linker symbol
stayed unique. It does not work: the JIT'd definition is not registered under the
label, and a call to it fails with `Couldn't look up symbols:
$__lldb_patched_probe_1`. Measured, not assumed.

Keeping the original name is better anyway. Backtraces read `target` with no
special casing, and reading the address out of the JIT module is more robust than
a name-based expression lookup, which would be ambiguous between the original and
the copy. The one caveat is that several JIT modules may end up defining the same
symbol after repeated re-patching; that only matters if a patched body references
the function by name, which happens only for direct recursion, and there
resolving to the newest patched copy is the desired behaviour.

## Open questions to resolve first in implementation

Does appending the JIT module with notification actually re-resolve `file:line`
breakpoints into the patched copy? This is the mechanism behind the whole
bookkeeping story, and it is the one claim in this document still unverified.

Two questions that were open in the first draft are now measured and closed.
`__atomic_*` builtins do compile and link in a top-level expression, with no
libcall emitted, so the control block's counters work as written. And the `asm`
label does not preserve a renamed symbol, which is why the section above exists.
