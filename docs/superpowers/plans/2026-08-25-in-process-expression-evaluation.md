# In-Process Expression Evaluation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Evaluate a tracepoint's condition and scalar captures inside the inferior, so a hit that does not need the debugger costs no stop.

**Architecture:** Read the enclosing function's body from source, inject the expression into that text, JIT it into the inferior as top-level code, and overwrite the original function's entry with a 16-byte trampoline to the copy. A true condition raises `__builtin_debugtrap()`, which LLDB attributes through a breakpoint site that never writes an opcode.

**Tech Stack:** C++17, LLDB (`lldbTarget`, `lldbBreakpoint`, `lldbExpression`), clang expression parser with `eExecutionPolicyTopLevel`, gtest, LLDB's Python API test suite.

**Spec:** `docs/superpowers/specs/2026-08-25-in-process-expression-evaluation-design.md`. Read it before starting — it records what was measured and why each choice was made.

## Global Constraints

- **arm64 only.** Every architecture-specific path must refuse other architectures rather than guess. Darwin is the only tested platform.
- **Build directory is `/Users/work/Developer/llvm/build`.** Unit tests: `ninja -C /Users/work/Developer/llvm/build TargetTests` then `/Users/work/Developer/llvm/build/tools/lldb/unittests/Target/TargetTests --gtest_filter=<Suite>.*`.
- **LLVM style.** Two-space indent, 80 columns, `///` doc comments on public declarations, `m_` prefix for private members, `PascalCase` for new local variables in new files under `lldb/source/Target` (match the file you are editing; existing LLDB code uses `snake_case` locals, new lldb-mcp-adjacent code in this branch uses `PascalCase` — follow the surrounding file).
- **No `Date.now()`-style nondeterminism in tests.** Golden-string tests must not embed addresses that vary between runs; pass addresses in as parameters.
- **Comments explain why, not what.** This branch's convention is strict about it. Never reference a task number, this plan, or a bug being fixed in a source comment.
- **Every failure falls back**, never aborts the run. The typed reason must reach the caller.
- **Commit after every task.** One commit per task, message in the imperative describing the rule established, not the mechanism.

## Verified facts the plan relies on

These were measured against a real arm64 process, not assumed. Do not re-litigate them; do re-verify if something behaves unexpectedly.

- `ldr x16, .+8` / `br x16` encode as `0x58000050` / `0xD61F0200`.
- `brk #0` leaves PC **at** the trap. `brk #0xf000` (`__builtin_debugtrap()`) leaves PC **past** it (+4). This is why the site is registered at `trap+4`.
- `expr --top-level` compiles a whole function body, resolving DWARF types and linking calls to program functions, with no headers supplied.
- `#line` redirects clang's debug info to the original file and lines.
- `__atomic_*` builtins compile and link in a top-level expression with no libcall.
- An `asm` label does **not** preserve a renamed JIT'd symbol. The generated function keeps the original name.
- Nothing currently registers a user expression's JIT module as a target image.

## File Structure

| File | Responsibility |
| --- | --- |
| `lldb/include/lldb/Target/EntryTrampoline.h`, `lldb/source/Target/EntryTrampoline.cpp` | arm64 trampoline encoding and the safety predicate. Pure. |
| `lldb/include/lldb/Target/FunctionBodySource.h`, `lldb/source/Target/FunctionBodySource.cpp` | Locate a function's textual body. Pure. |
| `lldb/include/lldb/Target/PatchControlBlock.h`, `lldb/source/Target/PatchControlBlock.cpp` | Inferior-side layout, record codec, ring drain. Pure. |
| `lldb/include/lldb/Target/PatchSourceBuilder.h`, `lldb/source/Target/PatchSourceBuilder.cpp` | Assemble the generated top-level source. Pure. |
| `lldb/include/lldb/Target/FunctionPatch.h`, `lldb/source/Target/FunctionPatch.cpp` | `FunctionPatchManager`: orchestration and all state. |
| `lldb/include/lldb/Breakpoint/BreakpointSite.h` | Add the `eProgramTrap` type. |
| `lldb/source/Target/Process.cpp`, `lldb/include/lldb/Target/Process.h` | Honour `eProgramTrap`; add a site-at-explicit-address entry point. |
| `lldb/source/Expression/LLVMUserExpression.cpp`, `lldb/include/lldb/Expression/LLVMUserExpression.h` | Expose the JIT module. |
| `lldb/source/Target/TargetProperties.td`, `lldb/source/Target/Target.cpp`, `lldb/include/lldb/Target/Target.h` | The opt-in setting and the manager's accessor. |
| `lldb/source/Breakpoint/BreakpointLocation.cpp` | Route a condition through the manager when the setting is on. |
| `lldb/source/Plugins/Protocol/MCP/ObservationEngine.{h,cpp}` | Install injections per observation, drain, report the mode. |
| `lldb/unittests/Target/*Test.cpp` | Unit tests for the pure pieces and the site type. |
| `lldb/test/API/functionalities/fast-conditions/` | End-to-end tests through `breakpoint set -c`. |

Tasks 1-4 are pure and independent of each other — they can be built in any order or in parallel. Task 5 is independent of 1-4. Tasks 6 onward depend on what came before.

---

### Task 1: arm64 entry trampoline encoder

Pure byte encoding plus the predicate that decides whether a function can be patched at all. No process required.

**Files:**
- Create: `lldb/include/lldb/Target/EntryTrampoline.h`
- Create: `lldb/source/Target/EntryTrampoline.cpp`
- Modify: `lldb/source/Target/CMakeLists.txt` (add `EntryTrampoline.cpp` to the `add_lldb_library(lldbTarget` source list, alphabetically after `DynamicRegisterInfo.cpp`)
- Create: `lldb/unittests/Target/EntryTrampolineTest.cpp`
- Modify: `lldb/unittests/Target/CMakeLists.txt` (add `EntryTrampolineTest.cpp` to the `add_lldb_unittest(TargetTests` source list, alphabetically after `ExecutionContextTest.cpp`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `constexpr size_t lldb_private::kEntryTrampolineSize = 16;`
  - `std::array<uint8_t, kEntryTrampolineSize> lldb_private::EncodeEntryTrampoline(lldb::addr_t Target);`

- [ ] **Step 1: Write the failing test**

Create `lldb/unittests/Target/EntryTrampolineTest.cpp`:

```cpp
//===-- EntryTrampolineTest.cpp -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/EntryTrampoline.h"
#include "gtest/gtest.h"
#include <cstring>

using namespace lldb_private;

// The reference encoding, taken from clang assembling
// `ldr x16, .+8` / `br x16` for arm64.
static constexpr uint8_t kLdrX16[4] = {0x50, 0x00, 0x00, 0x58};
static constexpr uint8_t kBrX16[4] = {0x00, 0x02, 0x1f, 0xd6};

TEST(EntryTrampolineTest, EncodesLoadAndBranch) {
  auto Bytes = EncodeEntryTrampoline(0x0000000100020000);
  EXPECT_EQ(0, std::memcmp(Bytes.data(), kLdrX16, 4));
  EXPECT_EQ(0, std::memcmp(Bytes.data() + 4, kBrX16, 4));
}

TEST(EntryTrampolineTest, EmbedsTargetLittleEndian) {
  auto Bytes = EncodeEntryTrampoline(0x8877665544332211);
  const uint8_t Expected[8] = {0x11, 0x22, 0x33, 0x44,
                               0x55, 0x66, 0x77, 0x88};
  EXPECT_EQ(0, std::memcmp(Bytes.data() + 8, Expected, 8));
}

// The literal is loaded from eight bytes past the `ldr`, so the two
// instructions and the literal must total exactly the patched width. A
// different size would mean the `ldr` reads the wrong bytes.
TEST(EntryTrampolineTest, IsSixteenBytes) {
  EXPECT_EQ(16u, kEntryTrampolineSize);
  auto Bytes = EncodeEntryTrampoline(0);
  EXPECT_EQ(16u, Bytes.size());
}

// A target of zero is still encoded rather than rejected: rejecting it here
// would put the check in the wrong place, since the caller knows whether an
// address is meaningful and this function only encodes.
TEST(EntryTrampolineTest, EncodesZeroTarget) {
  auto Bytes = EncodeEntryTrampoline(0);
  const uint8_t Expected[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(0, std::memcmp(Bytes.data() + 8, Expected, 8));
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
ninja -C /Users/work/Developer/llvm/build TargetTests
```

Expected: FAIL at compile time with `'lldb/Target/EntryTrampoline.h' file not found`.

- [ ] **Step 3: Write the header**

Create `lldb/include/lldb/Target/EntryTrampoline.h`:

```cpp
//===-- EntryTrampoline.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_TARGET_ENTRYTRAMPOLINE_H
#define LLDB_TARGET_ENTRYTRAMPOLINE_H

#include "lldb/lldb-types.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace lldb_private {

/// Bytes an entry trampoline occupies. The two instructions load the target
/// from the eight bytes that follow them, so the literal is part of the
/// patched region rather than stored elsewhere: a trampoline that reached
/// outside itself would need an allocation whose lifetime nothing owns.
constexpr size_t kEntryTrampolineSize = 16;

/// Encodes an arm64 branch to \p Target, to be written over a function's
/// entry.
///
/// Nothing is saved and nothing is restored. At a function's entry the
/// arguments are already in the registers the replacement expects, because it
/// has the same signature, and `x16` is architecturally scratch at a call
/// boundary -- it is reserved for exactly this, a linker's veneer. So the
/// branch is a jump rather than a call, and the replacement returns straight
/// to the original caller.
///
/// The target is loaded from a literal rather than encoded as a displacement,
/// which costs eight bytes and buys the whole address space: JIT'd code is
/// wherever the inferior's allocator put it, which is not reliably within
/// `b`'s 128MB reach.
std::array<uint8_t, kEntryTrampolineSize>
EncodeEntryTrampoline(lldb::addr_t Target);

} // namespace lldb_private

#endif // LLDB_TARGET_ENTRYTRAMPOLINE_H
```

- [ ] **Step 4: Write the implementation**

Create `lldb/source/Target/EntryTrampoline.cpp`:

```cpp
//===-- EntryTrampoline.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/EntryTrampoline.h"
#include "llvm/Support/Endian.h"

using namespace lldb_private;

namespace {
/// `ldr x16, .+8`: LDR (literal), 64-bit, imm19 of 2 (two instructions ahead),
/// Rt of 16.
constexpr uint32_t kLdrX16Literal = 0x58000050;

/// `br x16`.
constexpr uint32_t kBrX16 = 0xD61F0200;
} // namespace

std::array<uint8_t, kEntryTrampolineSize>
lldb_private::EncodeEntryTrampoline(lldb::addr_t Target) {
  std::array<uint8_t, kEntryTrampolineSize> Bytes = {};
  llvm::support::endian::write32le(Bytes.data(), kLdrX16Literal);
  llvm::support::endian::write32le(Bytes.data() + 4, kBrX16);
  llvm::support::endian::write64le(Bytes.data() + 8, Target);
  return Bytes;
}
```

- [ ] **Step 5: Register both files with CMake**

In `lldb/source/Target/CMakeLists.txt`, add `EntryTrampoline.cpp` to the `add_lldb_library(lldbTarget` source list, immediately after `DynamicRegisterInfo.cpp`.

In `lldb/unittests/Target/CMakeLists.txt`, add `EntryTrampolineTest.cpp` to the `add_lldb_unittest(TargetTests` source list, immediately after `ExecutionContextTest.cpp`.

- [ ] **Step 6: Run the tests to verify they pass**

```bash
ninja -C /Users/work/Developer/llvm/build TargetTests && \
  /Users/work/Developer/llvm/build/tools/lldb/unittests/Target/TargetTests --gtest_filter='EntryTrampolineTest.*'
```

Expected: `[  PASSED  ] 4 tests.`

- [ ] **Step 7: Commit**

```bash
git add lldb/include/lldb/Target/EntryTrampoline.h \
        lldb/source/Target/EntryTrampoline.cpp \
        lldb/source/Target/CMakeLists.txt \
        lldb/unittests/Target/EntryTrampolineTest.cpp \
        lldb/unittests/Target/CMakeLists.txt
git commit -m "[lldb] Redirect a function by loading its replacement's address, not by reaching it

A displacement would tie the replacement to within 128MB of the original, and
JIT'd code is wherever the inferior's allocator put it."
```

---

### Task 2: Locate a function's textual body

Pure text work. Given a source buffer and the line a function is declared on, return the byte range covering the whole declaration through its matching close brace, and refuse the cases that would produce a copy that misbehaves.

**Files:**
- Create: `lldb/include/lldb/Target/FunctionBodySource.h`
- Create: `lldb/source/Target/FunctionBodySource.cpp`
- Modify: `lldb/source/Target/CMakeLists.txt` (add `FunctionBodySource.cpp` after `ExecutionContext.cpp`)
- Create: `lldb/unittests/Target/FunctionBodySourceTest.cpp`
- Modify: `lldb/unittests/Target/CMakeLists.txt` (add `FunctionBodySourceTest.cpp` after `FindFileTest.cpp`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `enum class lldb_private::BodyExtractFailure { NotFound, Unbalanced, StaticLocal };`
  - `struct lldb_private::FunctionBodyText { std::string Text; uint32_t FirstLine; std::vector<size_t> LineStarts; };`
  - `llvm::Expected<FunctionBodyText> lldb_private::ExtractFunctionBody(llvm::StringRef Buffer, uint32_t DeclLine);`
  - `llvm::StringRef lldb_private::ToString(BodyExtractFailure);`

`FunctionBodyText::LineStarts` holds one offset per line of `Text`, relative to `Text`'s start, so `LineStarts[0]` is always 0 and line `FirstLine + N` begins at `LineStarts[N]`. Task 4 uses it to splice injections in by line.

- [ ] **Step 1: Write the failing test**

Create `lldb/unittests/Target/FunctionBodySourceTest.cpp`:

```cpp
//===-- FunctionBodySourceTest.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/FunctionBodySource.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"

using namespace lldb_private;

static llvm::Expected<FunctionBodyText> Extract(llvm::StringRef Buffer,
                                                uint32_t DeclLine) {
  return ExtractFunctionBody(Buffer, DeclLine);
}

TEST(FunctionBodySourceTest, ExtractsASimpleFunction) {
  llvm::StringRef Buffer = "int a;\n"
                           "int f(int x) {\n"
                           "  return x + 1;\n"
                           "}\n"
                           "int b;\n";
  auto Body = Extract(Buffer, 2);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_EQ("int f(int x) {\n  return x + 1;\n}", Body->Text);
  EXPECT_EQ(2u, Body->FirstLine);
}

TEST(FunctionBodySourceTest, IndexesEachLine) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  int y = 0;\n"
                           "  return y;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  ASSERT_EQ(4u, Body->LineStarts.size());
  EXPECT_EQ(0u, Body->LineStarts[0]);
  EXPECT_EQ("  int y = 0;\n  return y;\n}",
            Body->Text.substr(Body->LineStarts[1]));
  EXPECT_EQ("  return y;\n}", Body->Text.substr(Body->LineStarts[2]));
  EXPECT_EQ("}", Body->Text.substr(Body->LineStarts[3]));
}

TEST(FunctionBodySourceTest, CountsNestedBraces) {
  llvm::StringRef Buffer = "int f(int x) {\n"
                           "  if (x) {\n"
                           "    x = 2;\n"
                           "  }\n"
                           "  return x;\n"
                           "}\n"
                           "int after;\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_TRUE(llvm::StringRef(Body->Text).ends_with("return x;\n}"));
}

// A brace inside a string literal closes nothing. Without this the body would
// end early and the generated source would not compile.
TEST(FunctionBodySourceTest, IgnoresBracesInStringLiterals) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  puts(\"}\");\n"
                           "  return 0;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_TRUE(llvm::StringRef(Body->Text).ends_with("return 0;\n}"));
}

TEST(FunctionBodySourceTest, IgnoresBracesInCharLiterals) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  char c = '}';\n"
                           "  return c;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_TRUE(llvm::StringRef(Body->Text).ends_with("return c;\n}"));
}

TEST(FunctionBodySourceTest, IgnoresEscapedQuoteInsideString) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  puts(\"\\\"}\");\n"
                           "  return 0;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_TRUE(llvm::StringRef(Body->Text).ends_with("return 0;\n}"));
}

TEST(FunctionBodySourceTest, IgnoresBracesInLineComments) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  // }\n"
                           "  return 0;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_TRUE(llvm::StringRef(Body->Text).ends_with("return 0;\n}"));
}

TEST(FunctionBodySourceTest, IgnoresBracesInBlockComments) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  /* } */\n"
                           "  return 0;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_TRUE(llvm::StringRef(Body->Text).ends_with("return 0;\n}"));
}

// The copy would get storage of its own, diverging from the original's, so a
// body that keeps state between calls is refused rather than silently changed.
TEST(FunctionBodySourceTest, RefusesAStaticLocal) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  static int n = 0;\n"
                           "  return ++n;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  EXPECT_THAT_EXPECTED(Body, llvm::Failed());
}

// `static` naming a type or appearing inside a string is not storage.
TEST(FunctionBodySourceTest, AllowsStaticAsPartOfAnotherWord) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  int statics = 1;\n"
                           "  return statics;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  EXPECT_THAT_EXPECTED(Body, llvm::Succeeded());
}

TEST(FunctionBodySourceTest, FailsWhenBracesNeverBalance) {
  llvm::StringRef Buffer = "int f(void) {\n"
                           "  return 0;\n";
  auto Body = Extract(Buffer, 1);
  EXPECT_THAT_EXPECTED(Body, llvm::Failed());
}

TEST(FunctionBodySourceTest, FailsWhenNoBraceFollowsTheDeclaration) {
  llvm::StringRef Buffer = "int f(void);\n";
  auto Body = Extract(Buffer, 1);
  EXPECT_THAT_EXPECTED(Body, llvm::Failed());
}

TEST(FunctionBodySourceTest, FailsWhenTheLineIsPastTheBuffer) {
  llvm::StringRef Buffer = "int f(void) {}\n";
  auto Body = Extract(Buffer, 99);
  EXPECT_THAT_EXPECTED(Body, llvm::Failed());
}

// A declaration spanning several lines before its brace is ordinary in
// formatted code, so the search for the opening brace cannot stop at the
// declaration's own line.
TEST(FunctionBodySourceTest, HandlesAMultiLineDeclaration) {
  llvm::StringRef Buffer = "int f(int a,\n"
                           "      int b)\n"
                           "{\n"
                           "  return a + b;\n"
                           "}\n";
  auto Body = Extract(Buffer, 1);
  ASSERT_THAT_EXPECTED(Body, llvm::Succeeded());
  EXPECT_EQ(1u, Body->FirstLine);
  EXPECT_TRUE(llvm::StringRef(Body->Text).ends_with("return a + b;\n}"));
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
ninja -C /Users/work/Developer/llvm/build TargetTests
```

Expected: FAIL at compile time with `'lldb/Target/FunctionBodySource.h' file not found`.

- [ ] **Step 3: Write the header**

Create `lldb/include/lldb/Target/FunctionBodySource.h`:

```cpp
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
```

- [ ] **Step 4: Write the implementation**

Create `lldb/source/Target/FunctionBodySource.cpp`:

```cpp
//===-- FunctionBodySource.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/FunctionBodySource.h"
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

/// Whether \p Text contains the keyword `static` outside strings, characters
/// and comments.
///
/// A textual check, which is what keeps this callable before anything is
/// compiled. It over-reports in one direction only: a `static` that a macro
/// would have removed still refuses the patch, and refusing is the safe
/// answer.
bool DeclaresStatic(llvm::StringRef Text) {
  enum { Code, InString, InChar, InLineComment, InBlockComment } State = Code;
  for (size_t I = 0, E = Text.size(); I != E; ++I) {
    char C = Text[I];
    switch (State) {
    case Code:
      if (C == '"')
        State = InString;
      else if (C == '\'')
        State = InChar;
      else if (C == '/' && I + 1 < E && Text[I + 1] == '/')
        State = InLineComment;
      else if (C == '/' && I + 1 < E && Text[I + 1] == '*')
        State = InBlockComment;
      else if (C == 's' && Text.substr(I).starts_with("static")) {
        bool StartsWord = I == 0 || !(llvm::isAlnum(Text[I - 1]) ||
                                      Text[I - 1] == '_');
        size_t After = I + 6;
        bool EndsWord = After >= E || !(llvm::isAlnum(Text[After]) ||
                                        Text[After] == '_');
        if (StartsWord && EndsWord)
          return true;
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
      if (C == '*' && I + 1 < E && Text[I + 1] == '/') {
        ++I;
        State = Code;
      }
      break;
    }
  }
  return false;
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

  FunctionBodyText Body;
  Body.Text = Buffer.substr(Start, End - Start).str();
  Body.FirstLine = DeclLine;

  if (DeclaresStatic(Body.Text))
    return Fail(BodyExtractFailure::StaticLocal);

  Body.LineStarts.push_back(0);
  for (size_t I = 0, E = Body.Text.size(); I != E; ++I)
    if (Body.Text[I] == '\n' && I + 1 < E)
      Body.LineStarts.push_back(I + 1);

  return Body;
}
```

Note the `llvm::isAlnum` use needs `#include "llvm/ADT/StringExtras.h"`. Add it to the include list.

- [ ] **Step 5: Register both files with CMake**

In `lldb/source/Target/CMakeLists.txt`, add `FunctionBodySource.cpp` after `ExecutionContext.cpp`.

In `lldb/unittests/Target/CMakeLists.txt`, add `FunctionBodySourceTest.cpp` after `FindFileTest.cpp`.

- [ ] **Step 6: Run the tests to verify they pass**

```bash
ninja -C /Users/work/Developer/llvm/build TargetTests && \
  /Users/work/Developer/llvm/build/tools/lldb/unittests/Target/TargetTests --gtest_filter='FunctionBodySourceTest.*'
```

Expected: `[  PASSED  ] 14 tests.`

If `RefusesAStaticLocal` fails because `DeclaresStatic` never runs, check that it is called before `LineStarts` is filled — the order matters only for which error wins, but the test asserts failure.

- [ ] **Step 7: Commit**

```bash
git add lldb/include/lldb/Target/FunctionBodySource.h \
        lldb/source/Target/FunctionBodySource.cpp \
        lldb/source/Target/CMakeLists.txt \
        lldb/unittests/Target/FunctionBodySourceTest.cpp \
        lldb/unittests/Target/CMakeLists.txt
git commit -m "[lldb] Take a function's text verbatim, and refuse a body that keeps state

A brace inside a literal or a comment closes nothing, and a body whose storage
outlives a call cannot be copied without the copy diverging on the first call."
```

---

### Task 3: Control block layout, record codec, and ring drain

Pure. Defines the bytes the inferior and the debugger both read, and the arithmetic that turns a ring's contents into an ordered list plus a count of what was overwritten before anyone read it.

**Files:**
- Create: `lldb/include/lldb/Target/PatchControlBlock.h`
- Create: `lldb/source/Target/PatchControlBlock.cpp`
- Modify: `lldb/source/Target/CMakeLists.txt` (add `PatchControlBlock.cpp` after `PathMappingList.cpp`)
- Create: `lldb/unittests/Target/PatchControlBlockTest.cpp`
- Modify: `lldb/unittests/Target/CMakeLists.txt` (add `PatchControlBlockTest.cpp` after `PathMappingListTest.cpp`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `struct lldb_private::PatchRecord { uint32_t Site; uint32_t Capture; uint64_t Value; };`
  - `struct lldb_private::PatchRingHeader { uint64_t Seq, Drained, Capacity, HighWater; };`
  - `struct lldb_private::PatchSiteSlot { uint64_t Hits, CondTrue; uint8_t Gate; };`
  - `struct lldb_private::PatchDrain { std::vector<PatchRecord> Records; uint64_t Lost; uint64_t NewDrained; };`
  - `constexpr size_t lldb_private::kPatchRecordSize = 16;`
  - `constexpr size_t lldb_private::kPatchRingHeaderSize = 32;`
  - `constexpr size_t lldb_private::kPatchSiteSlotSize = 24;`
  - `constexpr uint64_t lldb_private::kDefaultRingCapacity = 4096;`
  - `PatchRecord lldb_private::DecodePatchRecord(llvm::ArrayRef<uint8_t> Bytes);`
  - `void lldb_private::EncodePatchRecord(const PatchRecord &Rec, llvm::MutableArrayRef<uint8_t> Out);`
  - `PatchDrain lldb_private::DrainPatchRing(const PatchRingHeader &Header, llvm::ArrayRef<uint8_t> RingBytes);`

The layouts must match what `PatchSourceBuilder` (Task 4) emits as C declarations. Both tasks state the same field order; a mismatch is caught by Task 4's test that asserts the emitted `struct` text against these sizes.

- [ ] **Step 1: Write the failing test**

Create `lldb/unittests/Target/PatchControlBlockTest.cpp`:

```cpp
//===-- PatchControlBlockTest.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/PatchControlBlock.h"
#include "gtest/gtest.h"
#include <vector>

using namespace lldb_private;

/// A ring holding \p Capacity records, with \p Written records stored at their
/// natural positions, so a test can describe a ring by what the inferior would
/// have put in it.
static std::vector<uint8_t> MakeRing(uint64_t Capacity,
                                     llvm::ArrayRef<PatchRecord> Written) {
  std::vector<uint8_t> Bytes(Capacity * kPatchRecordSize, 0);
  for (uint64_t I = 0; I < Written.size(); ++I) {
    uint64_t Slot = I % Capacity;
    EncodePatchRecord(Written[I],
                      llvm::MutableArrayRef<uint8_t>(
                          Bytes.data() + Slot * kPatchRecordSize,
                          kPatchRecordSize));
  }
  return Bytes;
}

TEST(PatchControlBlockTest, RoundTripsARecord) {
  PatchRecord Rec{7, 3, 0xDEADBEEFCAFEF00D};
  std::vector<uint8_t> Bytes(kPatchRecordSize, 0);
  EncodePatchRecord(Rec, Bytes);
  PatchRecord Back = DecodePatchRecord(Bytes);
  EXPECT_EQ(Rec.Site, Back.Site);
  EXPECT_EQ(Rec.Capture, Back.Capture);
  EXPECT_EQ(Rec.Value, Back.Value);
}

// The sizes are baked into the generated C source as literals, so a change here
// without a matching change there would misalign every record.
TEST(PatchControlBlockTest, HasTheDocumentedSizes) {
  EXPECT_EQ(16u, kPatchRecordSize);
  EXPECT_EQ(32u, kPatchRingHeaderSize);
  EXPECT_EQ(24u, kPatchSiteSlotSize);
}

TEST(PatchControlBlockTest, DrainsNothingFromAnUntouchedRing) {
  PatchRingHeader Header{0, 0, 8, 6};
  auto Ring = MakeRing(8, {});
  PatchDrain Drain = DrainPatchRing(Header, Ring);
  EXPECT_TRUE(Drain.Records.empty());
  EXPECT_EQ(0u, Drain.Lost);
  EXPECT_EQ(0u, Drain.NewDrained);
}

TEST(PatchControlBlockTest, DrainsRecordsInWriteOrder) {
  std::vector<PatchRecord> Written{{1, 0, 10}, {1, 1, 20}, {2, 0, 30}};
  auto Ring = MakeRing(8, Written);
  PatchRingHeader Header{3, 0, 8, 6};
  PatchDrain Drain = DrainPatchRing(Header, Ring);
  ASSERT_EQ(3u, Drain.Records.size());
  EXPECT_EQ(10u, Drain.Records[0].Value);
  EXPECT_EQ(20u, Drain.Records[1].Value);
  EXPECT_EQ(30u, Drain.Records[2].Value);
  EXPECT_EQ(0u, Drain.Lost);
  EXPECT_EQ(3u, Drain.NewDrained);
}

TEST(PatchControlBlockTest, DrainsOnlyWhatIsNew) {
  std::vector<PatchRecord> Written{{1, 0, 10}, {1, 0, 20}, {1, 0, 30}};
  auto Ring = MakeRing(8, Written);
  PatchRingHeader Header{3, 1, 8, 6};
  PatchDrain Drain = DrainPatchRing(Header, Ring);
  ASSERT_EQ(2u, Drain.Records.size());
  EXPECT_EQ(20u, Drain.Records[0].Value);
  EXPECT_EQ(30u, Drain.Records[1].Value);
  EXPECT_EQ(0u, Drain.Lost);
}

// Once more records have been written than the ring holds, the oldest are gone.
// How many is reported rather than absorbed, because a short list that looks
// complete is worse than a list that says what is missing.
TEST(PatchControlBlockTest, ReportsWhatTheRingOverwrote) {
  std::vector<PatchRecord> Written;
  for (uint64_t I = 0; I < 10; ++I)
    Written.push_back({1, 0, I});
  auto Ring = MakeRing(4, Written);
  PatchRingHeader Header{10, 0, 4, 3};
  PatchDrain Drain = DrainPatchRing(Header, Ring);
  EXPECT_EQ(4u, Drain.Records.size());
  EXPECT_EQ(6u, Drain.Lost);
  EXPECT_EQ(10u, Drain.NewDrained);
  // The four survivors are the four most recent, in write order.
  EXPECT_EQ(6u, Drain.Records[0].Value);
  EXPECT_EQ(7u, Drain.Records[1].Value);
  EXPECT_EQ(8u, Drain.Records[2].Value);
  EXPECT_EQ(9u, Drain.Records[3].Value);
}

TEST(PatchControlBlockTest, ReadsAcrossTheWrapPoint) {
  std::vector<PatchRecord> Written;
  for (uint64_t I = 0; I < 6; ++I)
    Written.push_back({1, 0, I});
  auto Ring = MakeRing(4, Written);
  PatchRingHeader Header{6, 3, 4, 3};
  PatchDrain Drain = DrainPatchRing(Header, Ring);
  ASSERT_EQ(3u, Drain.Records.size());
  EXPECT_EQ(3u, Drain.Records[0].Value);
  EXPECT_EQ(4u, Drain.Records[1].Value);
  EXPECT_EQ(5u, Drain.Records[2].Value);
  EXPECT_EQ(0u, Drain.Lost);
}

// A header claiming fewer records drained than written cannot be trusted to
// index the ring, but it must not read out of bounds either.
TEST(PatchControlBlockTest, ToleratesADrainedCountAheadOfSeq) {
  auto Ring = MakeRing(4, {{1, 0, 10}});
  PatchRingHeader Header{1, 5, 4, 3};
  PatchDrain Drain = DrainPatchRing(Header, Ring);
  EXPECT_TRUE(Drain.Records.empty());
  EXPECT_EQ(0u, Drain.Lost);
}

TEST(PatchControlBlockTest, ToleratesAZeroCapacity) {
  PatchRingHeader Header{4, 0, 0, 0};
  PatchDrain Drain = DrainPatchRing(Header, {});
  EXPECT_TRUE(Drain.Records.empty());
}

// A ring shorter than the header's capacity claims is a truncated read, not a
// reason to walk off the end of the buffer.
TEST(PatchControlBlockTest, ToleratesARingShorterThanCapacity) {
  PatchRingHeader Header{4, 0, 8, 6};
  auto Ring = MakeRing(2, {{1, 0, 10}, {1, 0, 20}});
  PatchDrain Drain = DrainPatchRing(Header, Ring);
  EXPECT_LE(Drain.Records.size(), 2u);
}

TEST(PatchControlBlockTest, DefaultCapacityIsAPowerOfTwo) {
  EXPECT_EQ(0u, kDefaultRingCapacity & (kDefaultRingCapacity - 1));
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
ninja -C /Users/work/Developer/llvm/build TargetTests
```

Expected: FAIL at compile time with `'lldb/Target/PatchControlBlock.h' file not found`.

- [ ] **Step 3: Write the header**

Create `lldb/include/lldb/Target/PatchControlBlock.h`:

```cpp
//===-- PatchControlBlock.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_TARGET_PATCHCONTROLBLOCK_H
#define LLDB_TARGET_PATCHCONTROLBLOCK_H

#include "llvm/ADT/ArrayRef.h"
#include <cstdint>
#include <vector>

namespace lldb_private {

/// One captured value, as the inferior writes it.
///
/// The value is eight raw bytes rather than a typed union: the capture's type
/// is read from the patched function's debug info, so encoding it here as well
/// would be a second source of truth that could disagree with the first.
struct PatchRecord {
  uint32_t Site = 0;
  uint32_t Capture = 0;
  uint64_t Value = 0;
};

/// The shared block's fixed head, which the record ring follows.
struct PatchRingHeader {
  /// Records the inferior has written, ever. Never reset, so it doubles as the
  /// ring index and as the count the debugger compares against.
  uint64_t Seq = 0;

  /// Records the debugger has read. Written by the debugger only.
  uint64_t Drained = 0;

  /// Records the ring holds. A power of two, so the inferior indexes with a
  /// mask rather than a division.
  uint64_t Capacity = 0;

  /// Unread records at which the inferior traps to ask for a drain.
  uint64_t HighWater = 0;
};

/// One site's own state, at its own address.
///
/// Deliberately not an array in the header. An array index's offset depends on
/// the array's length, so growing the site count would invalidate the offsets
/// already compiled into every patch running in the program, making the count a
/// hard limit fixed when the block was allocated.
struct PatchSiteSlot {
  uint64_t Hits = 0;
  uint64_t CondTrue = 0;
  /// Whether the site does anything at all. The debugger opens and closes it to
  /// implement gating that depends on the debugger's own state.
  uint8_t Gate = 0;
};

/// What one drain produced.
struct PatchDrain {
  /// New records, oldest first.
  std::vector<PatchRecord> Records;

  /// Records the ring overwrote before they were read. Reported rather than
  /// absorbed: a short list that looks complete is worse than one that says
  /// what is missing.
  uint64_t Lost = 0;

  /// What the debugger should store back as \ref PatchRingHeader::Drained.
  uint64_t NewDrained = 0;
};

constexpr size_t kPatchRecordSize = 16;
constexpr size_t kPatchRingHeaderSize = 32;
constexpr size_t kPatchSiteSlotSize = 24;

/// Records the ring holds by default: 64KB, which at three quarters full costs
/// one stop per three thousand captured values instead of one per value.
constexpr uint64_t kDefaultRingCapacity = 4096;

PatchRecord DecodePatchRecord(llvm::ArrayRef<uint8_t> Bytes);
void EncodePatchRecord(const PatchRecord &Rec,
                       llvm::MutableArrayRef<uint8_t> Out);

/// Reads every record written since \p Header.Drained out of \p RingBytes.
///
/// Tolerates a header that cannot be true -- a drained count ahead of the
/// sequence, a capacity of zero, a ring shorter than the capacity claims --
/// because the header was read from a live process and a torn read must not
/// become an out-of-bounds one.
PatchDrain DrainPatchRing(const PatchRingHeader &Header,
                          llvm::ArrayRef<uint8_t> RingBytes);

} // namespace lldb_private

#endif // LLDB_TARGET_PATCHCONTROLBLOCK_H
```

- [ ] **Step 4: Write the implementation**

Create `lldb/source/Target/PatchControlBlock.cpp`:

```cpp
//===-- PatchControlBlock.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/PatchControlBlock.h"
#include "llvm/Support/Endian.h"
#include <algorithm>

using namespace lldb_private;

PatchRecord lldb_private::DecodePatchRecord(llvm::ArrayRef<uint8_t> Bytes) {
  PatchRecord Rec;
  if (Bytes.size() < kPatchRecordSize)
    return Rec;
  Rec.Site = llvm::support::endian::read32le(Bytes.data());
  Rec.Capture = llvm::support::endian::read32le(Bytes.data() + 4);
  Rec.Value = llvm::support::endian::read64le(Bytes.data() + 8);
  return Rec;
}

void lldb_private::EncodePatchRecord(const PatchRecord &Rec,
                                    llvm::MutableArrayRef<uint8_t> Out) {
  if (Out.size() < kPatchRecordSize)
    return;
  llvm::support::endian::write32le(Out.data(), Rec.Site);
  llvm::support::endian::write32le(Out.data() + 4, Rec.Capture);
  llvm::support::endian::write64le(Out.data() + 8, Rec.Value);
}

PatchDrain lldb_private::DrainPatchRing(const PatchRingHeader &Header,
                                       llvm::ArrayRef<uint8_t> RingBytes) {
  PatchDrain Drain;
  Drain.NewDrained = Header.Drained;

  if (Header.Capacity == 0 || Header.Seq <= Header.Drained)
    return Drain;

  Drain.NewDrained = Header.Seq;

  // The ring keeps the most recent Capacity records, so anything older than
  // that is gone no matter what the debugger last read.
  const uint64_t Unread = Header.Seq - Header.Drained;
  const uint64_t Readable = std::min(Unread, Header.Capacity);
  Drain.Lost = Unread - Readable;

  const uint64_t Available = RingBytes.size() / kPatchRecordSize;
  const uint64_t First = Header.Seq - Readable;

  Drain.Records.reserve(Readable);
  for (uint64_t Seq = First; Seq < Header.Seq; ++Seq) {
    const uint64_t Slot = Seq % Header.Capacity;
    // A ring shorter than the capacity claims means the read was truncated.
    // Stopping is right; reading past the buffer is not.
    if (Slot >= Available)
      continue;
    Drain.Records.push_back(DecodePatchRecord(
        RingBytes.slice(Slot * kPatchRecordSize, kPatchRecordSize)));
  }
  return Drain;
}
```

- [ ] **Step 5: Register both files with CMake**

In `lldb/source/Target/CMakeLists.txt`, add `PatchControlBlock.cpp` after `PathMappingList.cpp`.

In `lldb/unittests/Target/CMakeLists.txt`, add `PatchControlBlockTest.cpp` after `PathMappingListTest.cpp`.

- [ ] **Step 6: Run the tests to verify they pass**

```bash
ninja -C /Users/work/Developer/llvm/build TargetTests && \
  /Users/work/Developer/llvm/build/tools/lldb/unittests/Target/TargetTests --gtest_filter='PatchControlBlockTest.*'
```

Expected: `[  PASSED  ] 11 tests.`

- [ ] **Step 7: Commit**

```bash
git add lldb/include/lldb/Target/PatchControlBlock.h \
        lldb/source/Target/PatchControlBlock.cpp \
        lldb/source/Target/CMakeLists.txt \
        lldb/unittests/Target/PatchControlBlockTest.cpp \
        lldb/unittests/Target/CMakeLists.txt
git commit -m "[lldb] Say how many captured values a ring overwrote, rather than returning the rest

A short list that looks complete is worse than one that reports what is
missing, and a header read from a live process may claim what cannot be true."
```

---

### Task 4: Assemble the generated top-level source

Pure. Turns a body's text plus a list of injections into the C source that gets JIT'd. This is where the `#line` discipline lives, and it is the only place that knows the shape of the injected code.

**Files:**
- Create: `lldb/include/lldb/Target/PatchSourceBuilder.h`
- Create: `lldb/source/Target/PatchSourceBuilder.cpp`
- Modify: `lldb/source/Target/CMakeLists.txt` (add `PatchSourceBuilder.cpp` after `PatchControlBlock.cpp`)
- Create: `lldb/unittests/Target/PatchSourceBuilderTest.cpp`
- Modify: `lldb/unittests/Target/CMakeLists.txt` (add `PatchSourceBuilderTest.cpp` after `PatchControlBlockTest.cpp`)

**Interfaces:**
- Consumes: `FunctionBodyText` (Task 2), `kPatchRecordSize` / `kPatchRingHeaderSize` / `kPatchSiteSlotSize` (Task 3).
- Produces:
  - `struct lldb_private::PatchInjection { uint32_t SiteID; uint32_t Line; lldb::addr_t SlotAddress; std::optional<std::string> Condition; std::vector<std::string> Captures; uint32_t SkipFirst; std::optional<uint32_t> OnlyHit; bool Gated; bool WantStop; };`
  - `struct lldb_private::PatchSourceRequest { FunctionBodyText Body; std::string SourcePath; lldb::addr_t RingAddress; uint64_t RingCapacity; std::vector<PatchInjection> Injections; };`
  - `std::string lldb_private::BuildPatchSource(const PatchSourceRequest &Request);`
  - `std::string lldb_private::CaptureLocalName(uint32_t SiteID, uint32_t Capture);`

`CaptureLocalName` is how Task 6 finds each capture's type in the JIT'd module's debug info, so both tasks must agree on the spelling. It returns `"__lldb_cap_<site>_<capture>"`.

**Emitted shape.** Injections are emitted before the line they attach to, each as exactly one physical line bracketed by two `#line` directives naming the same line, so the injected code and the original statement both map to that line. Everything on one physical line is what keeps the line accounting trivial.

- [ ] **Step 1: Write the failing test**

Create `lldb/unittests/Target/PatchSourceBuilderTest.cpp`:

```cpp
//===-- PatchSourceBuilderTest.cpp -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/PatchSourceBuilder.h"
#include "lldb/Target/FunctionBodySource.h"
#include "gtest/gtest.h"

using namespace lldb_private;

/// The body used throughout: `f` declared on line 4, one statement on line 5,
/// a return on line 6, closing brace on line 7.
static FunctionBodyText Body() {
  llvm::StringRef Buffer = "a\nb\nc\n"
                           "int f(int x) {\n"
                           "  int acc = x;\n"
                           "  return acc;\n"
                           "}\n";
  auto Extracted = ExtractFunctionBody(Buffer, 4);
  return cantFail(std::move(Extracted));
}

static PatchSourceRequest Request(std::vector<PatchInjection> Injections) {
  PatchSourceRequest Req;
  Req.Body = Body();
  Req.SourcePath = "/tmp/t.c";
  Req.RingAddress = 0x104000000;
  Req.RingCapacity = 4096;
  Req.Injections = std::move(Injections);
  return Req;
}

static PatchInjection Bare(uint32_t Line) {
  PatchInjection Inj;
  Inj.SiteID = 1;
  Inj.Line = Line;
  Inj.SlotAddress = 0x104010018;
  Inj.WantStop = true;
  return Inj;
}

TEST(PatchSourceBuilderTest, EmitsTheBodyVerbatimWithNoInjections) {
  std::string Source = BuildPatchSource(Request({}));
  EXPECT_NE(std::string::npos, Source.find("int f(int x) {\n"
                                           "  int acc = x;\n"
                                           "  return acc;\n"
                                           "}"));
}

TEST(PatchSourceBuilderTest, OpensTheBodyWithItsOwnFirstLine) {
  std::string Source = BuildPatchSource(Request({}));
  EXPECT_NE(std::string::npos,
            Source.find("#line 4 \"/tmp/t.c\"\nint f(int x) {"));
}

TEST(PatchSourceBuilderTest, BakesTheRingAddressAsALiteral) {
  std::string Source = BuildPatchSource(Request({}));
  EXPECT_NE(std::string::npos, Source.find("0x104000000"));
}

TEST(PatchSourceBuilderTest, BakesTheSiteSlotAddressAsALiteral) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("0x104010018"));
}

// Both the injected line and the statement it precedes claim the same line, so
// a stop just past the trap reports the line the caller asked about rather than
// the next one.
TEST(PatchSourceBuilderTest, BracketsAnInjectionWithMatchingLineDirectives) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  std::string Source = BuildPatchSource(Request({Inj}));
  size_t Before = Source.find("#line 5 \"/tmp/t.c\"\n");
  ASSERT_NE(std::string::npos, Before);
  size_t After = Source.find("#line 5 \"/tmp/t.c\"\n", Before + 1);
  ASSERT_NE(std::string::npos, After);
  EXPECT_NE(std::string::npos, Source.find("  int acc = x;", After));
}

// One physical line is what keeps the accounting trivial: two directives and
// one line between them, so no later line shifts.
TEST(PatchSourceBuilderTest, EmitsAnInjectionOnOnePhysicalLine) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  Inj.Captures = {"acc"};
  std::string Source = BuildPatchSource(Request({Inj}));
  size_t Start = Source.find("#line 5 \"/tmp/t.c\"\n");
  ASSERT_NE(std::string::npos, Start);
  Start += std::string("#line 5 \"/tmp/t.c\"\n").size();
  size_t End = Source.find('\n', Start);
  ASSERT_NE(std::string::npos, End);
  std::string Line = Source.substr(Start, End - Start);
  EXPECT_NE(std::string::npos, Line.find("__builtin_debugtrap()"));
  EXPECT_NE(std::string::npos, Line.find("__lldb_rec"));
}

TEST(PatchSourceBuilderTest, GuardsOnTheConditionWhenThereIsOne) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("if (acc > 1)"));
}

// A site with no condition still counts hits and still records, so the absence
// of a condition must not produce an `if` with nothing in it.
TEST(PatchSourceBuilderTest, OmitsTheConditionGuardWhenThereIsNone) {
  auto Inj = Bare(5);
  Inj.Captures = {"acc"};
  Inj.WantStop = false;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_EQ(std::string::npos, Source.find("if ()"));
  EXPECT_NE(std::string::npos, Source.find("__lldb_rec"));
}

TEST(PatchSourceBuilderTest, CountsHitsBeforeTestingTheCondition) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  std::string Source = BuildPatchSource(Request({Inj}));
  size_t Hits = Source.find("->hits");
  size_t Cond = Source.find("if (acc > 1)");
  ASSERT_NE(std::string::npos, Hits);
  ASSERT_NE(std::string::npos, Cond);
  EXPECT_LT(Hits, Cond);
}

TEST(PatchSourceBuilderTest, CountsConditionTruthInsideTheCondition) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  std::string Source = BuildPatchSource(Request({Inj}));
  size_t Cond = Source.find("if (acc > 1)");
  size_t True = Source.find("->cond_true");
  ASSERT_NE(std::string::npos, True);
  EXPECT_LT(Cond, True);
}

TEST(PatchSourceBuilderTest, ComparesTheHitCountAgainstSkipFirst) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  Inj.SkipFirst = 10;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("> 10"));
}

TEST(PatchSourceBuilderTest, ComparesTheHitCountForEqualityForOnlyHit) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  Inj.OnlyHit = 3;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("== 3"));
}

TEST(PatchSourceBuilderTest, ReadsTheGateWhenGated) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  Inj.Gated = true;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("->gate"));
}

TEST(PatchSourceBuilderTest, OmitsTheGateWhenNotGated) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  Inj.Gated = false;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_EQ(std::string::npos, Source.find("->gate"));
}

// A cast would truncate a double. The copy is bit-exact for every scalar
// because it is a memcpy of the value's own width.
TEST(PatchSourceBuilderTest, CopiesACaptureByItsOwnWidth) {
  auto Inj = Bare(5);
  Inj.Captures = {"acc"};
  Inj.WantStop = false;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("__typeof__(acc) __lldb_cap_1_0"));
  EXPECT_NE(std::string::npos,
            Source.find("__builtin_memcpy(&__lldb_v_1_0, &__lldb_cap_1_0, "
                        "sizeof __lldb_cap_1_0)"));
}

TEST(PatchSourceBuilderTest, NamesEachCaptureLocalDistinctly) {
  auto Inj = Bare(5);
  Inj.Captures = {"acc", "x"};
  Inj.WantStop = false;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("__lldb_cap_1_0"));
  EXPECT_NE(std::string::npos, Source.find("__lldb_cap_1_1"));
  EXPECT_EQ("__lldb_cap_1_0", CaptureLocalName(1, 0));
  EXPECT_EQ("__lldb_cap_1_1", CaptureLocalName(1, 1));
}

TEST(PatchSourceBuilderTest, TrapsWhenTheSiteWantsAStop) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  Inj.WantStop = true;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("__builtin_debugtrap()"));
}

// A site that only records must not stop, or every recorded value would cost
// the stop the recording exists to avoid.
TEST(PatchSourceBuilderTest, DoesNotTrapWhenTheSiteOnlyRecords) {
  auto Inj = Bare(5);
  Inj.Captures = {"acc"};
  Inj.WantStop = false;
  std::string Source = BuildPatchSource(Request({Inj}));
  // The only trap left is the drain trap, which tests the ring, not the
  // condition.
  size_t First = Source.find("__builtin_debugtrap()");
  ASSERT_NE(std::string::npos, First);
  EXPECT_NE(std::string::npos, Source.rfind("high_water", First));
}

TEST(PatchSourceBuilderTest, EmitsADrainTrapWhenThereAreCaptures) {
  auto Inj = Bare(5);
  Inj.Captures = {"acc"};
  Inj.WantStop = false;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("high_water"));
}

// Nothing is recorded, so nothing can fill the ring, so asking whether it is
// full would cost two loads per hit for an answer that cannot change.
TEST(PatchSourceBuilderTest, OmitsTheDrainTrapWithNoCaptures) {
  auto Inj = Bare(5);
  Inj.Condition = "acc > 1";
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_EQ(std::string::npos, Source.find("high_water"));
}

TEST(PatchSourceBuilderTest, PlacesTwoInjectionsAtTheirOwnLines) {
  auto First = Bare(5);
  First.SiteID = 1;
  First.Condition = "acc > 1";
  auto Second = Bare(6);
  Second.SiteID = 2;
  Second.SlotAddress = 0x104010030;
  Second.Condition = "acc > 2";
  std::string Source = BuildPatchSource(Request({First, Second}));
  size_t One = Source.find("if (acc > 1)");
  size_t Two = Source.find("if (acc > 2)");
  size_t Stmt = Source.find("  int acc = x;");
  size_t Ret = Source.find("  return acc;");
  ASSERT_NE(std::string::npos, One);
  ASSERT_NE(std::string::npos, Two);
  EXPECT_LT(One, Stmt);
  EXPECT_LT(Stmt, Two);
  EXPECT_LT(Two, Ret);
}

// Two conditions on the same line is what an already-patched function looks
// like when a second condition arrives at the same place.
TEST(PatchSourceBuilderTest, PlacesTwoInjectionsOnTheSameLine) {
  auto First = Bare(5);
  First.SiteID = 1;
  First.Condition = "acc > 1";
  auto Second = Bare(5);
  Second.SiteID = 2;
  Second.SlotAddress = 0x104010030;
  Second.Condition = "acc > 2";
  std::string Source = BuildPatchSource(Request({First, Second}));
  size_t One = Source.find("if (acc > 1)");
  size_t Two = Source.find("if (acc > 2)");
  size_t Stmt = Source.find("  int acc = x;");
  ASSERT_NE(std::string::npos, One);
  ASSERT_NE(std::string::npos, Two);
  EXPECT_LT(One, Two);
  EXPECT_LT(Two, Stmt);
}

// An injection is emitted in line order regardless of the order it was
// installed in, because it is spliced into text that only reads forwards.
TEST(PatchSourceBuilderTest, OrdersInjectionsByLineNotByArrival) {
  auto Late = Bare(6);
  Late.SiteID = 2;
  Late.Condition = "acc > 2";
  auto Early = Bare(5);
  Early.SiteID = 1;
  Early.Condition = "acc > 1";
  std::string Source = BuildPatchSource(Request({Late, Early}));
  EXPECT_LT(Source.find("if (acc > 1)"), Source.find("if (acc > 2)"));
}

// The declaration line and the closing brace are not statements, so an
// injection there has nowhere valid to go.
TEST(PatchSourceBuilderTest, DropsAnInjectionOutsideTheBody) {
  auto Before = Bare(1);
  Before.Condition = "acc > 1";
  auto After = Bare(99);
  After.SiteID = 2;
  After.Condition = "acc > 2";
  std::string Source = BuildPatchSource(Request({Before, After}));
  EXPECT_EQ(std::string::npos, Source.find("if (acc > 1)"));
  EXPECT_EQ(std::string::npos, Source.find("if (acc > 2)"));
}

TEST(PatchSourceBuilderTest, DeclaresTheRecordWriterBeforeTheBody) {
  auto Inj = Bare(5);
  Inj.Captures = {"acc"};
  Inj.WantStop = false;
  std::string Source = BuildPatchSource(Request({Inj}));
  size_t Helper = Source.find("static void __lldb_rec");
  size_t Fn = Source.find("int f(int x) {");
  ASSERT_NE(std::string::npos, Helper);
  ASSERT_NE(std::string::npos, Fn);
  EXPECT_LT(Helper, Fn);
}

// The capacity is a literal so the writer indexes with a mask and never loads
// it.
TEST(PatchSourceBuilderTest, MasksTheRingIndexWithALiteralCapacity) {
  auto Inj = Bare(5);
  Inj.Captures = {"acc"};
  Inj.WantStop = false;
  std::string Source = BuildPatchSource(Request({Inj}));
  EXPECT_NE(std::string::npos, Source.find("& 4095"));
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
ninja -C /Users/work/Developer/llvm/build TargetTests
```

Expected: FAIL at compile time with `'lldb/Target/PatchSourceBuilder.h' file not found`.

- [ ] **Step 3: Write the header**

Create `lldb/include/lldb/Target/PatchSourceBuilder.h`:

```cpp
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
```

- [ ] **Step 4: Write the implementation**

Create `lldb/source/Target/PatchSourceBuilder.cpp`:

```cpp
//===-- PatchSourceBuilder.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/PatchSourceBuilder.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>

using namespace lldb_private;

std::string lldb_private::CaptureLocalName(uint32_t SiteID, uint32_t Capture) {
  return llvm::formatv("__lldb_cap_{0}_{1}", SiteID, Capture).str();
}

namespace {

/// The declarations every patch needs, ahead of the body.
///
/// Attributed to a file of its own so that none of it claims a line of the
/// program's source, which would make a stop in the preamble report a line the
/// user could read but that says nothing.
std::string Preamble(const PatchSourceRequest &Request) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << "#line 1 \"<lldb patch preamble>\"\n";
  OS << "struct __lldb_rec_t { unsigned int site, cap; unsigned long long "
        "val; };\n";
  OS << "struct __lldb_hdr_t { unsigned long seq, drained, capacity, "
        "high_water; struct __lldb_rec_t ring[]; };\n";
  OS << "struct __lldb_site_t { unsigned long hits, cond_true; unsigned char "
        "gate; };\n";
  OS << llvm::formatv("#define __LLDB_HDR ((volatile struct __lldb_hdr_t "
                      "*){0:x+})\n",
                      Request.RingAddress);
  OS << llvm::formatv(
      "static void __lldb_rec(unsigned k, unsigned c, unsigned long long v) {{ "
      "unsigned long s = __atomic_fetch_add(&__LLDB_HDR->seq, 1, "
      "__ATOMIC_RELAXED); volatile struct __lldb_rec_t *r = "
      "&__LLDB_HDR->ring[s & {0}]; r->site = k; r->cap = c; r->val = v; }}\n",
      Request.RingCapacity ? Request.RingCapacity - 1 : 0);
  return Text;
}

/// One injection, as a single physical line.
std::string InjectionLine(const PatchInjection &Inj) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);

  const std::string Slot =
      llvm::formatv("((volatile struct __lldb_site_t *){0:x+})", Inj.SlotAddress)
          .str();
  const std::string Hit = llvm::formatv("__lldb_h_{0}", Inj.SiteID).str();

  if (Inj.Gated)
    OS << llvm::formatv("if ({0}->gate) {{ ", Slot);

  OS << llvm::formatv(
      "unsigned long {0} = __atomic_add_fetch(&{1}->hits, 1, "
      "__ATOMIC_RELAXED); ",
      Hit, Slot);
  OS << llvm::formatv("(void){0}; ", Hit);

  // A guard is emitted only when there is something to compare against, so a
  // site that records every hit does not pay for a branch that is always taken.
  std::string Guard;
  if (Inj.OnlyHit)
    Guard = llvm::formatv("{0} == {1}", Hit, *Inj.OnlyHit).str();
  else if (Inj.SkipFirst)
    Guard = llvm::formatv("{0} > {1}", Hit, Inj.SkipFirst).str();
  if (!Guard.empty())
    OS << llvm::formatv("if ({0}) {{ ", Guard);

  if (Inj.Condition)
    OS << llvm::formatv("if ({0}) {{ ", *Inj.Condition);

  if (Inj.Condition)
    OS << llvm::formatv("__atomic_add_fetch(&{0}->cond_true, 1, "
                        "__ATOMIC_RELAXED); ",
                        Slot);

  for (uint32_t I = 0; I < Inj.Captures.size(); ++I) {
    const std::string Local = CaptureLocalName(Inj.SiteID, I);
    const std::string Value =
        llvm::formatv("__lldb_v_{0}_{1}", Inj.SiteID, I).str();
    // A memcpy rather than a cast, because a cast converts and a double's bits
    // would not survive it. The width comes from the value itself, so every
    // scalar is handled by the same line.
    OS << llvm::formatv("__typeof__({0}) {1} = ({0}); ", Inj.Captures[I],
                        Local);
    OS << llvm::formatv("unsigned long long {0} = 0; ", Value);
    OS << llvm::formatv("__builtin_memcpy(&{0}, &{1}, sizeof {1}); ", Value,
                        Local);
    OS << llvm::formatv("__lldb_rec({0}, {1}, {2}); ", Inj.SiteID, I, Value);
  }

  if (Inj.WantStop)
    OS << "__builtin_debugtrap(); ";

  if (Inj.Condition)
    OS << "} ";
  if (!Guard.empty())
    OS << "} ";

  // Nothing is recorded without captures, so nothing can fill the ring, and
  // asking whether it is full would be two loads per hit for an answer that
  // cannot change.
  if (!Inj.Captures.empty())
    OS << "if (__LLDB_HDR->seq - __LLDB_HDR->drained >= "
          "__LLDB_HDR->high_water) __builtin_debugtrap(); ";

  if (Inj.Gated)
    OS << "}";

  return llvm::StringRef(Text).rtrim().str();
}

} // namespace

std::string lldb_private::BuildPatchSource(const PatchSourceRequest &Request) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << Preamble(Request);

  const uint32_t FirstLine = Request.Body.FirstLine;
  const size_t LineCount = Request.Body.LineStarts.size();
  const uint32_t LastLine =
      LineCount ? FirstLine + static_cast<uint32_t>(LineCount) - 1 : FirstLine;

  // Emitted in line order rather than arrival order, because the body is
  // spliced by reading forwards once.
  std::vector<const PatchInjection *> Ordered;
  for (const PatchInjection &Inj : Request.Injections) {
    // The declaration line and the closing brace are not statements. An
    // injection there is dropped rather than moved, since moving it would
    // report hits for a line nobody named.
    if (Inj.Line <= FirstLine || Inj.Line >= LastLine)
      continue;
    Ordered.push_back(&Inj);
  }
  std::stable_sort(Ordered.begin(), Ordered.end(),
                   [](const PatchInjection *A, const PatchInjection *B) {
                     return A->Line < B->Line;
                   });

  OS << llvm::formatv("#line {0} \"{1}\"\n", FirstLine, Request.SourcePath);

  size_t Next = 0;
  for (size_t Index = 0; Index < LineCount; ++Index) {
    const uint32_t Line = FirstLine + static_cast<uint32_t>(Index);

    while (Next < Ordered.size() && Ordered[Next]->Line == Line) {
      OS << llvm::formatv("#line {0} \"{1}\"\n", Line, Request.SourcePath);
      OS << InjectionLine(*Ordered[Next]) << "\n";
      ++Next;
    }

    // A directive is only needed where an injection has just moved the
    // compiler's idea of the current line.
    if (Next && Ordered[Next - 1]->Line == Line)
      OS << llvm::formatv("#line {0} \"{1}\"\n", Line, Request.SourcePath);

    const size_t Start = Request.Body.LineStarts[Index];
    const size_t End = Index + 1 < LineCount ? Request.Body.LineStarts[Index + 1]
                                             : Request.Body.Text.size();
    OS << llvm::StringRef(Request.Body.Text).slice(Start, End);
  }

  if (!llvm::StringRef(Text).ends_with("\n"))
    OS << "\n";
  return Text;
}
```

- [ ] **Step 5: Register both files with CMake**

In `lldb/source/Target/CMakeLists.txt`, add `PatchSourceBuilder.cpp` after `PatchControlBlock.cpp`.

In `lldb/unittests/Target/CMakeLists.txt`, add `PatchSourceBuilderTest.cpp` after `PatchControlBlockTest.cpp`.

- [ ] **Step 6: Run the tests to verify they pass**

```bash
ninja -C /Users/work/Developer/llvm/build TargetTests && \
  /Users/work/Developer/llvm/build/tools/lldb/unittests/Target/TargetTests --gtest_filter='PatchSourceBuilderTest.*'
```

Expected: `[  PASSED  ] 25 tests.`

Two failures to expect and fix rather than work around:
- `MasksTheRingIndexWithALiteralCapacity` fails if `formatv` renders the mask in hex. It must be decimal — `4095`, not `0xfff`.
- `BakesTheRingAddressAsALiteral` needs `{0:x+}` to render `0x104000000`. If it renders without the `0x`, the emitted C would be a decimal literal that happens to parse, so the test is what catches it.

- [ ] **Step 7: Commit**

```bash
git add lldb/include/lldb/Target/PatchSourceBuilder.h \
        lldb/source/Target/PatchSourceBuilder.cpp \
        lldb/source/Target/CMakeLists.txt \
        lldb/unittests/Target/PatchSourceBuilderTest.cpp \
        lldb/unittests/Target/CMakeLists.txt
git commit -m "[lldb] Let injected code claim the line it was attached to

The line a stop reports is read from the copy's debug info, so the injected
code and the statement it precedes both have to name the line the caller asked
about, or a stop past the trap would report the following statement."
```

---

### Task 5: A breakpoint site for a trap the program already contains

The core change, and smaller than it looks. Both memory-shadow paths in `Process` already gate on `eSoftware`, so a new type is excluded from read and write shadowing with no edit. What remains is the enum value, an entry point that registers a site without writing an opcode, and teaching the resume path not to step over an instruction that is not a trap.

**Why this type has to exist.** `PC == trap+4` arises two ways: the trap fired and the kernel advanced PC, or the condition was false and the branch skipped there. A normal site at `trap+4` would write its own trap and fire in *both* cases, which is every hit — defeating the feature. A site that never writes an opcode is only ever reached through the exception the program raised itself. It is a label for attributing a trap the code raised, not a mechanism for causing one.

**Files:**
- Modify: `lldb/include/lldb/Breakpoint/BreakpointSite.h` (the `Type` enum, around line 37)
- Modify: `lldb/include/lldb/Target/Process.h` (declare `CreateProgramTrapSite` next to `CreateBreakpointSite`, around line 2350)
- Modify: `lldb/source/Target/Process.cpp` (implement it after `CreateBreakpointSite`, which ends around line 1808)
- Modify: `lldb/source/Target/Thread.cpp:666` (skip the step-over plan)
- Create: `lldb/unittests/Breakpoint/BreakpointSiteTest.cpp`
- Modify: `lldb/unittests/Breakpoint/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `BreakpointSite::eProgramTrap` enumerator.
  - `lldb::break_id_t Process::CreateProgramTrapSite(const lldb::BreakpointLocationSP &Constituent, lldb::addr_t Addr);`

**Test coverage boundary, stated rather than implied.** The unit test covers the enum and a site's own state. That `CreateProgramTrapSite` writes no opcode, and that the resume path does not step over it, are process-level behaviours; they are covered by the API test in Task 9. Do not invent a mock that asserts them here — `lldb/unittests/Target/MemoryTest.cpp` has a `DummyProcess` plus `TargetHack` pattern if a later task needs one, but bending it into a breakpoint test costs more than the API test that covers it properly.

- [ ] **Step 1: Write the failing test**

Check whether `lldb/unittests/Breakpoint/CMakeLists.txt` exists:

```bash
ls lldb/unittests/Breakpoint/
```

If the directory does not exist, add the test to `lldb/unittests/Target/CMakeLists.txt` as `BreakpointSiteTest.cpp` instead and create it at `lldb/unittests/Target/BreakpointSiteTest.cpp`. The rest of this task is unchanged apart from the path.

Create the test:

```cpp
//===-- BreakpointSiteTest.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Breakpoint/BreakpointSite.h"
#include "gtest/gtest.h"

using namespace lldb_private;

// The three existing types are distinguished by who owns the trap: lldb wrote
// it, the hardware holds it, or a remote stub manages it. A trap the program
// itself contains is a fourth answer, so it needs a value of its own rather
// than reusing one whose enable path would install a trap.
TEST(BreakpointSiteTest, ProgramTrapIsItsOwnType) {
  EXPECT_NE(BreakpointSite::eProgramTrap, BreakpointSite::eSoftware);
  EXPECT_NE(BreakpointSite::eProgramTrap, BreakpointSite::eHardware);
  EXPECT_NE(BreakpointSite::eProgramTrap, BreakpointSite::eExternal);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
ninja -C /Users/work/Developer/llvm/build TargetTests
```

Expected: FAIL at compile time with `no member named 'eProgramTrap' in 'lldb_private::BreakpointSite'`.

- [ ] **Step 3: Add the enum value**

In `lldb/include/lldb/Breakpoint/BreakpointSite.h`, extend the `Type` enum. The existing `eExternal` comment ends with a description of transparent memory reads; add after it:

```cpp
    eExternal, // Breakpoint site is managed by an external debug nub or
               // debug interface where memory reads transparently will not
               // display any breakpoint opcodes.
    eProgramTrap // The trap is already in the program's own code, so there is
                 // nothing to write and nothing to restore. The site exists to
                 // attribute a trap the program raised, not to cause one:
                 // registering a site that wrote its own trap here would fire
                 // on every pass over the address rather than only when the
                 // program's trap executed.
```

Note the comma after `eExternal` — it is currently the last enumerator and has none.

- [ ] **Step 4: Run the test to verify it passes**

```bash
ninja -C /Users/work/Developer/llvm/build TargetTests && \
  /Users/work/Developer/llvm/build/tools/lldb/unittests/Target/TargetTests --gtest_filter='BreakpointSiteTest.*'
```

Expected: `[  PASSED  ] 1 test.`

- [ ] **Step 5: Declare the site-creating entry point**

In `lldb/include/lldb/Target/Process.h`, immediately after the existing declaration:

```cpp
  lldb::break_id_t CreateBreakpointSite(const lldb::BreakpointLocationSP &owner,
                                        bool use_hardware);
```

add:

```cpp
  /// Registers a site at \p addr for a trap the program's own code contains.
  ///
  /// Unlike \ref CreateBreakpointSite, the address is given rather than taken
  /// from \p constituent: the trap is wherever the compiler put it, which is
  /// not where the location that owns it resolved. Nothing is written to the
  /// inferior, because the trap is already there.
  lldb::break_id_t
  CreateProgramTrapSite(const lldb::BreakpointLocationSP &constituent,
                        lldb::addr_t addr);
```

- [ ] **Step 6: Implement it**

In `lldb/source/Target/Process.cpp`, after `Process::CreateBreakpointSite` ends (just before `Process::RemoveConstituentFromBreakpointSite`):

```cpp
lldb::break_id_t
Process::CreateProgramTrapSite(const BreakpointLocationSP &constituent,
                               addr_t addr) {
  if (addr == LLDB_INVALID_ADDRESS)
    return LLDB_INVALID_BREAK_ID;

  if (BreakpointSiteSP bp_site_sp =
          m_breakpoint_site_list.FindByAddress(addr)) {
    bp_site_sp->AddConstituent(constituent);
    constituent->SetBreakpointSite(bp_site_sp);
    return bp_site_sp->GetID();
  }

  BreakpointSiteSP bp_site_sp(
      new BreakpointSite(constituent, addr, /*use_hardware=*/false));
  bp_site_sp->SetType(BreakpointSite::eProgramTrap);

  // Enabled without being installed. The trap is in the program's own text, so
  // there is no action that would make it fire and none that would stop it;
  // saying the site is enabled is what lets a stop at its address be
  // attributed to it.
  bp_site_sp->SetEnabled(true);

  constituent->SetBreakpointSite(bp_site_sp);
  return m_breakpoint_site_list.Add(bp_site_sp);
}
```

If `SetType` or `SetEnabled` is inaccessible, `Process` needs to be a friend of `BreakpointSite`. Check for an existing `friend class Process;` near `friend class StopInfoBreakpoint;` in `BreakpointSite.h` and add one if it is absent.

- [ ] **Step 7: Stop the resume path from stepping over it**

In `lldb/source/Target/Thread.cpp`, the condition at line 666 currently reads:

```cpp
      if (bp_site_sp && m_stopped_at_unexecuted_bp != thread_pc) {
```

Change it to:

```cpp
      // A trap the program contains has already been executed by the time the
      // stop arrives -- the kernel leaves pc past it -- so the instruction at
      // pc is an ordinary one. Stepping over it would skip a real instruction.
      const bool is_program_trap =
          bp_site_sp && bp_site_sp->GetType() == BreakpointSite::eProgramTrap;
      if (bp_site_sp && !is_program_trap &&
          m_stopped_at_unexecuted_bp != thread_pc) {
```

- [ ] **Step 8: Build lldb and confirm nothing regressed**

```bash
ninja -C /Users/work/Developer/llvm/build lldb TargetTests && \
  /Users/work/Developer/llvm/build/tools/lldb/unittests/Target/TargetTests
```

Expected: the whole `TargetTests` suite passes. The new enumerator must not have changed behaviour for any existing type, so a failure here means a `switch` somewhere now has an unhandled case — build warnings will name it.

- [ ] **Step 9: Commit**

```bash
git add lldb/include/lldb/Breakpoint/BreakpointSite.h \
        lldb/include/lldb/Target/Process.h \
        lldb/source/Target/Process.cpp \
        lldb/source/Target/Thread.cpp \
        lldb/unittests/Target/BreakpointSiteTest.cpp \
        lldb/unittests/Target/CMakeLists.txt
git commit -m "[lldb] Let a site attribute a trap the program raised, without causing one

A site that writes its own trap fires on every pass over its address. A trap
compiled into the program fires only when the program executes it, so the site
that names it must install nothing -- and must not be stepped over, since the
kernel leaves pc past the trap and the instruction there is an ordinary one."
```

---

### Task 6: Register a top-level expression's JIT module with the target

The gap found while validating the design: nothing currently makes a user expression's JIT'd code visible to LLDB as a module. `LLVMUserExpression::m_jit_module_wp` is declared and never assigned; only `FunctionCaller` appends one. Measured consequence — a stop inside JIT'd code reports no function and no line, even when the source was compiled with `#line` directives and debug info.

Two things depend on closing it. The patched copy needs a symbol so its address and its captures' types can be read, and the module has to be appended *with notification* so LLDB re-resolves breakpoints into it — which is how a `file:line` breakpoint set after patching gains a location in the copy.

**Files:**
- Modify: `lldb/include/lldb/Expression/LLVMUserExpression.h` (add the accessor in the `public:` section, after `Text()` around line 72)
- Modify: `lldb/source/Expression/LLVMUserExpression.cpp` (implement near the destructor, which already touches `m_jit_module_wp` around line 58)

**Interfaces:**
- Consumes: nothing.
- Produces: `lldb::ModuleSP LLVMUserExpression::TakeJITModule();`

**Why a `Take` rather than a `Get`.** The module is created on demand from the execution unit, and creating it twice would produce two modules describing the same code. The name says the caller owns the result and must not ask again.

- [ ] **Step 1: Declare the accessor**

In `lldb/include/lldb/Expression/LLVMUserExpression.h`, after:

```cpp
  /// Return the string that the parser should parse.  Must be a full
  /// translation unit.
  const char *Text() override { return m_transformed_text.c_str(); }
```

add:

```cpp
  /// Builds a module describing this expression's JIT'd code, so its symbols
  /// and line table become visible to the rest of lldb.
  ///
  /// Nothing does this for a user expression otherwise, which is why code
  /// compiled with debug info still reports no function and no line when
  /// stopped inside it. Only useful for a top-level expression, whose code
  /// outlives the evaluation that produced it.
  ///
  /// Takes rather than gets: the module is built on demand, so asking twice
  /// would describe the same code twice. The caller owns the result, including
  /// whether to append it to the target and whether to notify.
  lldb::ModuleSP TakeJITModule();
```

- [ ] **Step 2: Implement it**

In `lldb/source/Expression/LLVMUserExpression.cpp`, add after the destructor:

```cpp
lldb::ModuleSP LLVMUserExpression::TakeJITModule() {
  if (!m_execution_unit_sp)
    return nullptr;
  lldb::ModuleSP jit_module_sp = m_execution_unit_sp->GetJITModule();
  if (jit_module_sp)
    m_jit_module_wp = jit_module_sp;
  return jit_module_sp;
}
```

The destructor already removes `m_jit_module_wp` from the target's images if it was set, so recording it here is what keeps a module appended by the caller from outliving the expression that owns its memory.

- [ ] **Step 3: Build**

```bash
ninja -C /Users/work/Developer/llvm/build lldb
```

Expected: builds clean. If `GetJITModule` is not declared, add `#include "lldb/Expression/IRExecutionUnit.h"` — check the existing includes first, it is likely already there since the destructor uses the execution unit.

- [ ] **Step 4: Verify against a real process**

This has no unit test, because its whole content is an interaction with a live JIT. Verify it directly, and keep the script — Task 7 needs the same harness.

Create `/tmp/ipe-verify/probe.py`:

```python
import sys
sys.path.insert(0, "/Users/work/Developer/llvm/build/lib/python3.14/site-packages")
import lldb

dbg = lldb.SBDebugger.Create(); dbg.SetAsync(False)
t = dbg.CreateTarget("/tmp/ipe-verify/t")
t.BreakpointCreateByName("main")
p = t.LaunchSimple(None, None, "/tmp/ipe-verify")

before = t.GetNumModules()
opts = lldb.SBExpressionOptions()
opts.SetTopLevel(True); opts.SetGenerateDebugInfo(True)
opts.SetLanguage(lldb.eLanguageTypeC99)
src = ('#line 4 "/tmp/ipe-verify/t.c"\n'
       'int probe_fn(int v) { return v + 1; }\n')
t.EvaluateExpression(src, opts)
print("modules before/after:", before, t.GetNumModules())
```

with `/tmp/ipe-verify/t.c`:

```c
#include <stdio.h>
int helper(int v) { return v * 3; }
int main(void) { printf("%d\n", helper(2)); return 0; }
```

built with `clang -g -O0 -arch arm64 /tmp/ipe-verify/t.c -o /tmp/ipe-verify/t`.

Run: `/usr/bin/python3 /tmp/ipe-verify/probe.py`

Expected: the module count is unchanged, because this task only *offers* the module — nothing appends it yet. That is the correct result here; Task 7 is what appends it. The point of running it is to confirm the build still evaluates top-level expressions at all.

- [ ] **Step 5: Commit**

```bash
git add lldb/include/lldb/Expression/LLVMUserExpression.h \
        lldb/source/Expression/LLVMUserExpression.cpp
git commit -m "[lldb] Offer an expression's JIT'd code as a module

Code compiled with debug info still reports no function and no line when
stopped inside it, because nothing describes it to the rest of lldb. A
top-level expression's code outlives the evaluation, so it is worth describing."
```

---

### Task 7: The patch manager — install a condition-only patch

Where everything meets: read the source, build the text, JIT it, install the trampoline, register the trap site. Captures come in Task 10 and re-patching in Task 9, so this task's `Install` handles a condition and nothing else.

**A layering note that costs one small move.** The refusal on a source file newer than its binary needs the same threshold the MCP plugin already uses, but `DescribeSourceSkew` lives in a plugin and `lldbTarget` cannot depend on one. Only the *decision* is shared; the plugin's formatting is its own. So the two-second noise floor and the boolean move to `lldbTarget`, and `DescribeSourceSkew` calls it. That keeps one home for the threshold rather than two that can drift.

**Files:**
- Create: `lldb/include/lldb/Target/FunctionPatch.h`
- Create: `lldb/source/Target/FunctionPatch.cpp`
- Modify: `lldb/source/Target/CMakeLists.txt` (add `FunctionPatch.cpp` after `FunctionBodySource.cpp`)
- Modify: `lldb/include/lldb/Target/Target.h` (declare `GetFunctionPatchManager`)
- Modify: `lldb/source/Target/Target.cpp` (define it; add the member)
- Modify: `lldb/source/Plugins/Protocol/MCP/ObservationPlan.cpp:959,998` (call the moved predicate)
- Create: `lldb/unittests/Target/FunctionPatchTest.cpp`
- Modify: `lldb/unittests/Target/CMakeLists.txt`

**Interfaces:**
- Consumes: `EncodeEntryTrampoline`, `kEntryTrampolineSize` (Task 1); `ExtractFunctionBody`, `FunctionBodyText` (Task 2); `PatchRingHeader`, `PatchSiteSlot`, `kPatchRingHeaderSize`, `kPatchSiteSlotSize`, `kDefaultRingCapacity`, `kPatchRecordSize` (Task 3); `BuildPatchSource`, `PatchInjection`, `PatchSourceRequest` (Task 4); `Process::CreateProgramTrapSite` (Task 5); `LLVMUserExpression::TakeJITModule` (Task 6).
- Produces:
  - `enum class lldb_private::PatchFailure { NotArm64, NoProcess, NoSourceFile, SourceNewerThanBinary, BodyNotFound, StaticLocal, EntryTooSmall, ThreadInPatchRange, BreakpointInPatchRange, CompileFailed, CaptureNotScalar, Unsupported };`
  - `llvm::StringRef lldb_private::ToString(PatchFailure);`
  - `bool lldb_private::SourceSkewExceedsNoise(llvm::sys::TimePoint<> Source, llvm::sys::TimePoint<> Binary);`
  - `struct lldb_private::PatchRequest { lldb::addr_t FunctionEntry; uint32_t Line; std::optional<std::string> Condition; std::vector<std::string> Captures; uint32_t SkipFirst; std::optional<uint32_t> OnlyHit; bool Gated; bool WantStop; BreakpointHitCallback OnTrap; void *Baton; };`
  - `class lldb_private::FunctionPatchManager` with `llvm::Expected<uint32_t> Install(const PatchRequest &)`, `llvm::Error Remove(uint32_t)`, `void SetGate(uint32_t, bool)`, `bool IsPatched(lldb::addr_t) const`.
  - `FunctionPatchManager &Target::GetFunctionPatchManager();`

- [ ] **Step 1: Write the failing test for the shared predicate**

Add to a new `lldb/unittests/Target/FunctionPatchTest.cpp`:

```cpp
//===-- FunctionPatchTest.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/FunctionPatch.h"
#include "gtest/gtest.h"
#include <chrono>

using namespace lldb_private;
using llvm::sys::TimePoint;

static TimePoint<> At(long Seconds) {
  return TimePoint<>() + std::chrono::seconds(Seconds);
}

// A build writes the binary after the sources it read, and a filesystem with
// coarse timestamps can leave the two within a second of each other either way.
// Refusing on that noise would refuse every ordinary build.
TEST(FunctionPatchTest, IgnoresSkewWithinTheNoiseFloor) {
  EXPECT_FALSE(SourceSkewExceedsNoise(At(1000), At(1000)));
  EXPECT_FALSE(SourceSkewExceedsNoise(At(1001), At(1000)));
}

TEST(FunctionPatchTest, DetectsSourceWrittenAfterTheBinary) {
  EXPECT_TRUE(SourceSkewExceedsNoise(At(1010), At(1000)));
}

TEST(FunctionPatchTest, IgnoresSourceOlderThanTheBinary) {
  EXPECT_FALSE(SourceSkewExceedsNoise(At(1000), At(1010)));
}

// An unreadable mtime arrives as the epoch. A comparison that cannot be made is
// not a finding, and refusing on it would refuse every binary built elsewhere.
TEST(FunctionPatchTest, IgnoresAnUnreadableTimestamp) {
  EXPECT_FALSE(SourceSkewExceedsNoise(TimePoint<>(), At(1000)));
  EXPECT_FALSE(SourceSkewExceedsNoise(At(1000), TimePoint<>()));
}

// Every reason is nameable, because a report that says a patch was refused
// without saying why leaves the caller unable to act on it.
TEST(FunctionPatchTest, NamesEveryFailure) {
  const PatchFailure All[] = {
      PatchFailure::NotArm64,           PatchFailure::NoProcess,
      PatchFailure::NoSourceFile,       PatchFailure::SourceNewerThanBinary,
      PatchFailure::BodyNotFound,       PatchFailure::StaticLocal,
      PatchFailure::EntryTooSmall,      PatchFailure::ThreadInPatchRange,
      PatchFailure::BreakpointInPatchRange, PatchFailure::CompileFailed,
      PatchFailure::CaptureNotScalar,   PatchFailure::Unsupported};
  for (PatchFailure Reason : All) {
    EXPECT_FALSE(ToString(Reason).empty());
    EXPECT_NE("unknown", ToString(Reason));
  }
}
```

- [ ] **Step 2: Run it to verify it fails**

```bash
ninja -C /Users/work/Developer/llvm/build TargetTests
```

Expected: FAIL at compile time with `'lldb/Target/FunctionPatch.h' file not found`.

- [ ] **Step 3: Write the header**

Create `lldb/include/lldb/Target/FunctionPatch.h`:

```cpp
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
```

- [ ] **Step 4: Write the parts the unit test covers**

Create `lldb/source/Target/FunctionPatch.cpp` with the pure parts first, so the unit test passes before any process work exists:

```cpp
//===-- FunctionPatch.cpp -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/FunctionPatch.h"
#include "lldb/Target/Target.h"
#include <chrono>

using namespace lldb_private;

namespace {
/// A build writes the binary after the sources it read, and a filesystem with
/// coarse timestamps can leave the two within a second of each other either
/// way. A refusal that fires on that noise would refuse every ordinary build.
constexpr std::chrono::seconds kMinSourceSkew{2};
} // namespace

llvm::StringRef lldb_private::ToString(PatchFailure Reason) {
  switch (Reason) {
  case PatchFailure::NotArm64:
    return "in-process evaluation is implemented for arm64 only";
  case PatchFailure::NoProcess:
    return "there is no running process to patch";
  case PatchFailure::NoSourceFile:
    return "the function's source file could not be read";
  case PatchFailure::SourceNewerThanBinary:
    return "the source on disk was written after the binary, so recompiling "
           "it would substitute a different function";
  case PatchFailure::BodyNotFound:
    return "the function's body could not be located in its source";
  case PatchFailure::StaticLocal:
    return "the body declares a static local, which a copy cannot share";
  case PatchFailure::EntryTooSmall:
    return "the function is too small to hold a redirect";
  case PatchFailure::ThreadInPatchRange:
    return "a thread is stopped inside the bytes the redirect would overwrite";
  case PatchFailure::BreakpointInPatchRange:
    return "a breakpoint sits inside the bytes the redirect would overwrite";
  case PatchFailure::CompileFailed:
    return "the recompiled function did not compile";
  case PatchFailure::CaptureNotScalar:
    return "the capture is not a scalar of eight bytes or fewer";
  case PatchFailure::Unsupported:
    return "the observation asks for something in-process evaluation does not "
           "implement";
  }
  return "unknown";
}

bool lldb_private::SourceSkewExceedsNoise(llvm::sys::TimePoint<> Source,
                                         llvm::sys::TimePoint<> Binary) {
  if (Source == llvm::sys::TimePoint<>() || Binary == llvm::sys::TimePoint<>())
    return false;
  return Source - Binary >= kMinSourceSkew;
}
```

- [ ] **Step 5: Register with CMake and confirm the unit test passes**

Add `FunctionPatch.cpp` to `lldb/source/Target/CMakeLists.txt` after `FunctionBodySource.cpp`, and `FunctionPatchTest.cpp` to `lldb/unittests/Target/CMakeLists.txt` after `FunctionBodySourceTest.cpp`.

```bash
ninja -C /Users/work/Developer/llvm/build TargetTests && \
  /Users/work/Developer/llvm/build/tools/lldb/unittests/Target/TargetTests --gtest_filter='FunctionPatchTest.*'
```

Expected: `[  PASSED  ] 5 tests.`

- [ ] **Step 6: Commit the pure part**

```bash
git add lldb/include/lldb/Target/FunctionPatch.h \
        lldb/source/Target/FunctionPatch.cpp \
        lldb/source/Target/CMakeLists.txt \
        lldb/unittests/Target/FunctionPatchTest.cpp \
        lldb/unittests/Target/CMakeLists.txt
git commit -m "[lldb] Name every reason a function cannot be patched

A fallback that does not say why it fell back leaves the caller unable to act
on it, so each refusal is a value with prose of its own."
```

- [ ] **Step 7: Give the threshold one home**

In `lldb/source/Plugins/Protocol/MCP/ObservationPlan.cpp`, delete the `MinSourceSkew` constant at line 959 along with its comment, and replace the two guards in `DescribeSourceSkew`:

```cpp
  if (Source == sys::TimePoint<>() || Binary == sys::TimePoint<>())
    return std::nullopt;
  if (Source - Binary < MinSourceSkew)
    return std::nullopt;
```

with:

```cpp
  if (!SourceSkewExceedsNoise(Source, Binary))
    return std::nullopt;
```

Add `#include "lldb/Target/FunctionPatch.h"` to that file's includes.

```bash
ninja -C /Users/work/Developer/llvm/build ProtocolTests && \
  /Users/work/Developer/llvm/build/tools/lldb/unittests/Protocol/ProtocolTests --gtest_filter='*SourceSkew*:*Skew*'
```

Expected: the existing skew tests still pass. If none match that filter, run the whole suite: `/Users/work/Developer/llvm/build/tools/lldb/unittests/Protocol/ProtocolTests`.

- [ ] **Step 8: Commit the move**

```bash
git add lldb/source/Plugins/Protocol/MCP/ObservationPlan.cpp
git commit -m "[lldb] Decide source skew in one place, and describe it in another

The threshold below which skew is a build's own noise is the same question for
a refusal and for a report, so two copies of it could disagree about the same
pair of files."
```

- [ ] **Step 9: Implement the install path**

Append to `lldb/source/Target/FunctionPatch.cpp`. The structure, in order — each of these is a refusal point, and none of them aborts a run:

1. `PatchedFunction` definition: original entry, the `FunctionBodyText`, source path, the live `PatchInjection` list, the saved 16 original bytes, the current copy's address, the current JIT module, retired modules, and the site ids it owns.
2. `Install`: refuse unless the target's triple is arm64 (`PatchFailure::NotArm64`) and there is a live, stopped process (`NoProcess`).
3. Find or create the `PatchedFunction`. On first sight of a function: resolve `FunctionEntry` to a `Function` via `Target::ResolveLoadAddress` and `SymbolContext`; read `DW_AT_decl_file` and `decl_line` from it; read the file through `Target::GetSourceManager()`; compare the source's mtime against the module's file mtime with `SourceSkewExceedsNoise` (`SourceNewerThanBinary`); call `ExtractFunctionBody` (`BodyNotFound` / `StaticLocal`, mapped from `BodyExtractFailure`); confirm the function's size is at least `kEntryTrampolineSize` (`EntryTooSmall`); confirm no thread's PC is within `[Entry, Entry + kEntryTrampolineSize)` (`ThreadInPatchRange`) and no breakpoint site is (`BreakpointInPatchRange`); save the original 16 bytes with `Process::ReadMemory`.
4. `EnsureRingBlock`: on first install, `Process::AllocateMemory(kPatchRingHeaderSize + kDefaultRingCapacity * kPatchRecordSize, ePermissionsReadable | ePermissionsWritable)`, then write a zeroed header with `Capacity = kDefaultRingCapacity` and `HighWater = kDefaultRingCapacity * 3 / 4`.
5. `AllocateSiteSlot`: hand out `kPatchSiteSlotSize` from a pool page, allocating another `Process::AllocateMemory(4096, ...)` when the current one is exhausted. Zero the slot, then set `Gate` to 1 unless the request is gated — an ungated site's gate is never read, but a gated one starts closed.
6. Append the `PatchInjection` (site id from `m_next_site_id++`, slot address from step 5) to the function's list, then `Recompile`.
7. `Recompile`: `BuildPatchSource` over the original body and every live injection; evaluate it with `Target::EvaluateExpression` under options with `SetExecutionPolicy(eExecutionPolicyTopLevel)`, `SetGenerateDebugInfo(true)`, `SetLanguage(eLanguageTypeC99)`, `SetIgnoreBreakpoints(true)`, `SetTryAllThreads(false)`, `SetUnwindOnError(true)`. A non-empty diagnostic is `CompileFailed`, carrying the diagnostic text. Then take the JIT module with `TakeJITModule()` and append it to `m_target.GetImages()` **with notification**, so breakpoints re-resolve into it. Find the patched function's address by looking up the original function's name in that module's symbol table — not by evaluating `&name`, which is ambiguous once a copy exists. Write `EncodeEntryTrampoline(copy_address)` over the original entry with `Process::WriteMemory`. Finally, for each injection, find its trap addresses and register each with `Process::CreateProgramTrapSite` at `trap + 4`.
8. Finding a trap address: disassemble the copy's range and collect every `brk #0xf000` (`0xD43E0000`), in address order. The order matches the order `BuildPatchSource` emitted them — injections sorted by line, and within one injection the stop trap before the drain trap. Assert the count matches what was emitted; a mismatch means the builder and this reader disagree, which is a bug rather than a refusal.

Write this incrementally, building after each of steps 2, 4, 6, and 7, since a compile error in one obscures the rest.

- [ ] **Step 10: Add the accessor on Target**

In `lldb/include/lldb/Target/Target.h`, in the public section, add:

```cpp
  /// The functions this target has had patched so that tracepoint work happens
  /// inside the process.
  FunctionPatchManager &GetFunctionPatchManager();
```

and a private member:

```cpp
  std::unique_ptr<FunctionPatchManager> m_function_patch_manager_up;
```

with a forward declaration `class FunctionPatchManager;` near the other forward declarations. In `Target.cpp`:

```cpp
FunctionPatchManager &Target::GetFunctionPatchManager() {
  if (!m_function_patch_manager_up)
    m_function_patch_manager_up = std::make_unique<FunctionPatchManager>(*this);
  return *m_function_patch_manager_up;
}
```

Add `#include "lldb/Target/FunctionPatch.h"` to `Target.cpp`.

- [ ] **Step 11: Build**

```bash
ninja -C /Users/work/Developer/llvm/build lldb TargetTests && \
  /Users/work/Developer/llvm/build/tools/lldb/unittests/Target/TargetTests
```

Expected: builds clean, whole suite passes.

- [ ] **Step 12: Commit**

```bash
git add lldb/include/lldb/Target/FunctionPatch.h \
        lldb/source/Target/FunctionPatch.cpp \
        lldb/include/lldb/Target/Target.h \
        lldb/source/Target/Target.cpp
git commit -m "[lldb] Redirect a function to a copy of itself compiled with the condition in it

The copy is compiled from the function's own source, so the condition sees the
locals as locals and needs nothing marshalled to reach them. The redirect goes
at the entry whatever line the condition attaches to, which is what makes it
free of any register to save."
```

---

### Task 8: Wire it to conditional breakpoints, and prove the point

The first task whose test measures the feature's purpose rather than its mechanism: a condition that is false a hundred thousand times must cost no stops.

**Files:**
- Modify: `lldb/source/Target/TargetProperties.td` (add to the `target_experimental` block, which starts at line 3)
- Modify: `lldb/include/lldb/Target/Target.h` (declare the getter near `GetUseDIL` at line 280)
- Modify: `lldb/source/Target/Target.cpp` (define it near `GetUseDIL` at line 5205)
- Modify: `lldb/source/Breakpoint/BreakpointLocation.cpp` (`SetCondition`)
- Create: `lldb/test/API/functionalities/fast-conditions/Makefile`
- Create: `lldb/test/API/functionalities/fast-conditions/main.c`
- Create: `lldb/test/API/functionalities/fast-conditions/TestFastConditions.py`

**Interfaces:**
- Consumes: `Target::GetFunctionPatchManager` and `PatchRequest` (Task 7).
- Produces: `bool TargetProperties::GetFastConditions(ExecutionContext *) const;`

- [ ] **Step 1: Add the setting**

In `lldb/source/Target/TargetProperties.td`, inside the `target_experimental` block, after the `UseDIL` property:

```
  def FastConditions : Property<"fast-conditions", "Boolean">,
    Global, DefaultFalse,
    Desc<"If true, compile a breakpoint's condition into the process by recompiling the enclosing function, so a condition that does not hold costs no stop. The patched function is compiled without optimization, so its timing and in some cases its behaviour differ from the original.">;
```

Default false: the copy is an unoptimized recompile, which is a change to the program under test, so it is opted into rather than out of.

- [ ] **Step 2: Add the getter**

In `lldb/include/lldb/Target/Target.h`, after the `GetUseDIL` declaration:

```cpp
  bool GetFastConditions(ExecutionContext *exe_ctx) const;
```

In `lldb/source/Target/Target.cpp`, after `GetUseDIL`:

```cpp
bool TargetProperties::GetFastConditions(ExecutionContext *exe_ctx) const {
  return GetExperimentalPropertyValue(ePropertyFastConditions, exe_ctx)
      .value_or(false);
}
```

- [ ] **Step 3: Build and confirm the setting exists**

```bash
ninja -C /Users/work/Developer/llvm/build lldb && \
  /Users/work/Developer/llvm/build/bin/lldb -b -o "settings show target.experimental.fast-conditions"
```

Expected: `target.experimental.fast-conditions (boolean) = false`

- [ ] **Step 4: Write the failing API test**

Create `lldb/test/API/functionalities/fast-conditions/main.c`:

```c
#include <stdio.h>

int accumulate(int seed, int rounds) {
  int total = seed;
  for (int i = 0; i < rounds; i++) {
    total += i;
  }
  return total;
}

int main(void) {
  long sum = 0;
  for (int k = 0; k < 100000; k++)
    sum += accumulate(k, 3);
  printf("sum=%ld\n", sum);
  return 0;
}
```

Create `lldb/test/API/functionalities/fast-conditions/Makefile`:

```make
C_SOURCES := main.c

include Makefile.rules
```

Create `lldb/test/API/functionalities/fast-conditions/TestFastConditions.py`:

```python
"""
A breakpoint condition compiled into the process, so that a condition which does
not hold costs no stop.
"""

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil

# The program calls the observed function this many times.
CALLS = 100000


class FastConditionsTestCase(TestBase):
    NO_DEBUG_INFO_TESTCASE = True

    def setup(self):
        self.build()
        target = self.dbg.CreateTarget(self.getBuildArtifact("a.out"))
        self.assertTrue(target, VALID_TARGET)
        self.runCmd("settings set target.experimental.fast-conditions true")
        return target

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_false_condition_never_stops(self):
        """A condition that never holds must not stop the process at all.

        This is the whole point of the feature, so it is the first thing
        asserted: with the condition evaluated at a stop, this run would pay
        100,000 stops.
        """
        target = self.setup()
        # The seed equals the loop counter, so it reaches CALLS - 1 at most and
        # this condition can never hold.
        bp = target.BreakpointCreateBySourceRegex(
            "total += i;", lldb.SBFileSpec("main.c")
        )
        self.assertGreater(bp.GetNumLocations(), 0, "condition location resolved")
        bp.SetCondition("seed > 1000000")

        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        self.assertState(process.GetState(), lldb.eStateExited)
        self.assertEqual(process.GetExitStatus(), 0)
        self.assertEqual(bp.GetHitCount(), 0, "no hit was reported")

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_true_condition_stops_at_the_right_line(self):
        """A condition that holds stops, reporting the original source."""
        target = self.setup()
        bp = target.BreakpointCreateBySourceRegex(
            "total += i;", lldb.SBFileSpec("main.c")
        )
        bp.SetCondition("seed == 40000 && i == 2")

        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        self.assertState(process.GetState(), lldb.eStateStopped)

        thread = process.GetSelectedThread()
        frame = thread.GetFrameAtIndex(0)
        # The line comes from the copy's own line table, which the #line
        # directives point back at the original file.
        self.assertEqual(frame.GetLineEntry().GetFileSpec().GetFilename(), "main.c")
        self.assertIn("total += i;", frame.GetLineEntry().GetLine() and
                      self.get_source_line(frame.GetLineEntry().GetLine()))
        # The locals are the patched function's own, so they read normally.
        self.assertEqual(frame.FindVariable("seed").GetValueAsSigned(), 40000)
        self.assertEqual(frame.FindVariable("i").GetValueAsSigned(), 2)
        # The caller is intact, which is what the entry redirect preserves.
        self.assertEqual(thread.GetFrameAtIndex(1).GetFunctionName(), "main")

    def get_source_line(self, line):
        with open(os.path.join(self.getSourceDir(), "main.c")) as f:
            return f.readlines()[line - 1]

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_falls_back_without_the_setting(self):
        """With the setting off, nothing is patched and the condition still works.

        The fallback is the existing path, so this asserts the feature is opt-in
        rather than asserting how the condition was evaluated.
        """
        self.build()
        target = self.dbg.CreateTarget(self.getBuildArtifact("a.out"))
        self.runCmd("settings set target.experimental.fast-conditions false")
        bp = target.BreakpointCreateBySourceRegex(
            "total += i;", lldb.SBFileSpec("main.c")
        )
        bp.SetCondition("seed == 5 && i == 1")
        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        self.assertState(process.GetState(), lldb.eStateStopped)
        self.assertEqual(
            process.GetSelectedThread().GetFrameAtIndex(0).FindVariable(
                "seed").GetValueAsSigned(), 5)
```

Add `import os` to the imports.

- [ ] **Step 5: Run it to verify it fails**

```bash
/Users/work/Developer/llvm/build/bin/lldb-dotest -p TestFastConditions.py
```

Expected: `test_false_condition_never_stops` FAILS with a hit count of 100000, because nothing routes the condition through the manager yet. The other two should already pass — the third by using the old path, the second because the old path also stops at the right line. A failure in the third means the test harness itself is wrong; fix that before going on.

- [ ] **Step 6: Route the condition through the manager**

In `lldb/source/Breakpoint/BreakpointLocation.cpp`, in `SetCondition`, after the existing body stores the condition, attempt the patch. The location knows its own address and its breakpoint's callback, which is what the trap site needs to forward to:

```cpp
  // With the setting on, compile the condition into the process instead of
  // evaluating it at a stop. A hit whose condition does not hold then costs
  // nothing, which is the difference between a condition that can be left on a
  // hot line and one that cannot.
  //
  // Every reason this can fail leaves the condition to be evaluated at a stop,
  // so nothing here changes whether the condition works -- only what it costs.
  ExecutionContext exe_ctx(GetTarget().shared_from_this(), false);
  if (!condition_text.empty() && GetTarget().GetFastConditions(&exe_ctx)) {
    if (llvm::Error Err = TryInProcessCondition(condition_text))
      LLDB_LOG_ERROR(GetLog(LLDBLog::Breakpoints), std::move(Err),
                     "fast condition unavailable: {0}");
  }
```

with a private helper on `BreakpointLocation`:

```cpp
llvm::Error BreakpointLocation::TryInProcessCondition(llvm::StringRef Text) {
  Address Resolved = GetAddress();
  SymbolContext SC;
  Resolved.CalculateSymbolContext(&SC);
  if (!SC.function)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "no function at the location");

  PatchRequest Request;
  Request.FunctionEntry =
      SC.function->GetAddressRange().GetBaseAddress().GetLoadAddress(
          &GetTarget());
  Request.Line = GetAddress().CalculateSymbolContextLineEntry().line;
  Request.Condition = Text.str();
  Request.WantStop = true;

  llvm::Expected<uint32_t> SiteID =
      GetTarget().GetFunctionPatchManager().Install(Request);
  if (!SiteID)
    return SiteID.takeError();

  // The patched copy carries the hits now, so the location's own trap comes
  // down: leaving it would stop on every pass through the original body, which
  // the redirect has made unreachable anyway.
  m_in_process_site_id = *SiteID;
  SetEnabled(false);
  return llvm::Error::success();
}
```

Declare `m_in_process_site_id` as a `std::optional<uint32_t>` member and `TryInProcessCondition` as a private method in `lldb/include/lldb/Breakpoint/BreakpointLocation.h`. Add includes for `lldb/Target/FunctionPatch.h` and `lldb/Symbol/SymbolContext.h`.

The trap site's callback must forward to this location so hit counts and callbacks keep working. Set `Request.OnTrap` to a static function that takes the location as its baton and calls `GetBreakpoint().GetOptions().InvokeCallback(...)` plus `BumpHitCount` equivalent — mirror what `StopInfoBreakpoint` does for an ordinary site. If forwarding proves awkward, having the site own its own internal `Breakpoint` and bumping this location's counts from its callback is the acceptable simpler shape; say which one was used in the commit message.

- [ ] **Step 7: Run the test to verify it passes**

```bash
ninja -C /Users/work/Developer/llvm/build lldb && \
  /Users/work/Developer/llvm/build/bin/lldb-dotest -p TestFastConditions.py
```

Expected: all three tests pass. `test_false_condition_never_stops` is the one that matters — a hit count of 0 over 100,000 calls.

If it still reports 100,000 hits, the patch was refused. Find out why rather than guessing: the log names the reason.

```bash
/Users/work/Developer/llvm/build/bin/lldb -b \
  -o "log enable -f /tmp/bp.log lldb breakpoints" \
  -o "settings set target.experimental.fast-conditions true" \
  -o "breakpoint set -f main.c -l 6 -c 'seed > 1000000'" \
  -o "run" /path/to/a.out
```

- [ ] **Step 8: Commit**

```bash
git add lldb/source/Target/TargetProperties.td \
        lldb/include/lldb/Target/Target.h \
        lldb/source/Target/Target.cpp \
        lldb/include/lldb/Breakpoint/BreakpointLocation.h \
        lldb/source/Breakpoint/BreakpointLocation.cpp \
        lldb/test/API/functionalities/fast-conditions/
git commit -m "[lldb] Let a condition that does not hold cost nothing

A condition evaluated at a stop is paid for on every hit, which is what keeps
one off a hot line. Compiled into the process it is paid for only when it
holds, so the interesting places stop being the unaffordable ones."
```

---

### Task 9: Patch a function that is already patched

The requirement is that a second condition on an already-patched function ends up with both conditions in the program's text. The manager never patches a patch: it rebuilds from the original source with every live injection and re-points the trampoline.

**Files:**
- Modify: `lldb/source/Target/FunctionPatch.cpp` (`Install`, `Remove`, `Recompile`)
- Modify: `lldb/include/lldb/Target/FunctionPatch.h` (the retired-module list on `PatchedFunction`)
- Modify: `lldb/test/API/functionalities/fast-conditions/TestFastConditions.py`

**Interfaces:**
- Consumes: everything from Task 7.
- Produces: no new public names. `Install` on an already-patched function now recompiles rather than refusing, and `Remove` recompiles what remains.

- [ ] **Step 1: Write the failing test**

Add to `TestFastConditions.py`:

```python
    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_two_conditions_in_one_function(self):
        """A second condition on a patched function keeps the first one working.

        Both end up compiled into the same copy, because the copy is rebuilt
        from the original source rather than patched again.
        """
        target = self.setup()
        first = target.BreakpointCreateBySourceRegex(
            "total += i;", lldb.SBFileSpec("main.c")
        )
        first.SetCondition("seed == 10 && i == 1")
        second = target.BreakpointCreateBySourceRegex(
            "return total;", lldb.SBFileSpec("main.c")
        )
        second.SetCondition("seed == 20")

        process = target.LaunchSimple(None, None, self.get_process_working_directory())

        # The first condition holds at seed 10, before the second at seed 20.
        self.assertState(process.GetState(), lldb.eStateStopped)
        frame = process.GetSelectedThread().GetFrameAtIndex(0)
        self.assertEqual(frame.FindVariable("seed").GetValueAsSigned(), 10)

        process.Continue()
        self.assertState(process.GetState(), lldb.eStateStopped)
        frame = process.GetSelectedThread().GetFrameAtIndex(0)
        self.assertEqual(frame.FindVariable("seed").GetValueAsSigned(), 20)

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_removing_one_condition_leaves_the_other(self):
        """Dropping one injection recompiles what remains."""
        target = self.setup()
        first = target.BreakpointCreateBySourceRegex(
            "total += i;", lldb.SBFileSpec("main.c")
        )
        first.SetCondition("seed == 10 && i == 1")
        second = target.BreakpointCreateBySourceRegex(
            "return total;", lldb.SBFileSpec("main.c")
        )
        second.SetCondition("seed == 20")
        target.BreakpointDelete(first.GetID())

        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        self.assertState(process.GetState(), lldb.eStateStopped)
        frame = process.GetSelectedThread().GetFrameAtIndex(0)
        self.assertEqual(frame.FindVariable("seed").GetValueAsSigned(), 20)
```

- [ ] **Step 2: Run to verify it fails**

```bash
/Users/work/Developer/llvm/build/bin/lldb-dotest -p TestFastConditions.py
```

Expected: `test_two_conditions_in_one_function` FAILS. How it fails depends on what Task 7 left: either the second `Install` refuses because the function is already patched, or it recompiles and the first site's trap addresses are stale, so the first condition stops with no site registered and surfaces as `EXC_BREAKPOINT`.

- [ ] **Step 3: Make `Install` rebuild rather than refuse**

In `Install`, when `m_functions` already holds the entry, skip every step that reads the source or saves the original bytes — those are already recorded — and go straight to appending the new `PatchInjection` and calling `Recompile`. The original body text is what `Recompile` reads, so a second injection composes with the first by construction.

- [ ] **Step 4: Retire the previous copy rather than dropping it**

In `Recompile`, before installing the new trampoline target:

1. Move the current JIT module and its trap site ids onto the function's retired list. Do not remove the modules from the target's image list and do not unregister the sites. A call already on the stack is still executing inside the old copy, so its frames need the module to unwind and its traps need somewhere to be attributed.
2. Register the new copy's sites.
3. Rewrite only the trampoline's 8-byte literal at `Entry + 8`, not all 16 bytes. The literal is 8-byte aligned, so this is a single store and there is no window in which a half-written trampoline is reachable.

A retired copy's sites keep counting into the same slots, because the slot addresses are baked in and unchanged. That is correct: it is the same site, and an in-flight call's hit belongs to it.

- [ ] **Step 5: Make `Remove` leave a retired copy safe to finish**

`Remove` drops the injection from the live list and recompiles. The retired copy still contains that injection's trap, and after removal nothing wants to stop there, so replace the retired site's callback with one that resumes without reporting anything. Unregistering the site instead would let a straggler surface as a bare `EXC_BREAKPOINT`, which is a worse outcome than an ignored stop.

Add to `PatchedFunction`:

```cpp
    /// Copies replaced by a later recompile. Kept alive, with their sites still
    /// registered, because a call already on the stack is still running inside
    /// one: its frames need the module to unwind, and its traps need a site to
    /// be attributed to rather than surfacing as a bare exception.
    std::vector<lldb::ModuleSP> RetiredModules;
    std::vector<lldb::break_id_t> RetiredSites;
```

- [ ] **Step 6: Run the tests**

```bash
ninja -C /Users/work/Developer/llvm/build lldb && \
  /Users/work/Developer/llvm/build/bin/lldb-dotest -p TestFastConditions.py
```

Expected: all five tests pass.

- [ ] **Step 7: Commit**

```bash
git add lldb/include/lldb/Target/FunctionPatch.h \
        lldb/source/Target/FunctionPatch.cpp \
        lldb/test/API/functionalities/fast-conditions/TestFastConditions.py
git commit -m "[lldb] Compile a second condition from the function's source, not into its copy

Patching a patch would compound whatever the first pass got wrong and would
have no original left to fall back to. Rebuilding from the source the function
was compiled from keeps every condition a first-class one, and leaves the
copy it replaces alive for the calls still running inside it."
```

---

### Task 10: Record scalar captures, and read them back as typed values

A capture's value has to leave the process without a stop. The injected code writes it to the ring; the debugger drains at the traps the injected code raises, and rebuilds a typed value from eight raw bytes plus the type recorded in the copy's debug info.

**Files:**
- Modify: `lldb/include/lldb/Target/FunctionPatch.h` (`Drain`, `PatchDrainResult`, the per-site capture types)
- Modify: `lldb/source/Target/FunctionPatch.cpp`
- Modify: `lldb/unittests/Target/FunctionPatchTest.cpp`
- Modify: `lldb/test/API/functionalities/fast-conditions/TestFastConditions.py`

**Interfaces:**
- Consumes: `DrainPatchRing`, `PatchDrain`, `PatchRingHeader`, `PatchSiteSlot` (Task 3); `CaptureLocalName` (Task 4); the manager (Task 7).
- Produces:
  - `struct lldb_private::CapturedValue { uint32_t SiteID; uint32_t Capture; lldb::ValueObjectSP Value; };`
  - `struct lldb_private::SiteCounters { uint64_t Hits; uint64_t CondTrue; };`
  - `struct lldb_private::PatchDrainResult { std::vector<CapturedValue> Values; llvm::DenseMap<uint32_t, SiteCounters> Counters; uint64_t Lost; };`
  - `llvm::Expected<PatchDrainResult> FunctionPatchManager::Drain();`

- [ ] **Step 1: Recover each capture's type at install time**

In `Recompile`, after the copy compiles and its module is appended, look up each capture's local by the name `CaptureLocalName(SiteID, Index)` in the patched function's block in that module's debug info, and record its `CompilerType` against `(SiteID, Index)`.

Refuse the capture with `PatchFailure::CaptureNotScalar` unless the type is a scalar of eight bytes or fewer — `CompilerType::IsScalarType()` and `GetByteSize()`. This is the gate the spec puts after compiling rather than before, because `__typeof__` is what determines the type and only the compiler knows it.

A refused capture must not refuse the whole injection: drop that capture, recompile without it, and report the reason for that capture alone. Otherwise one aggregate in a capture list would cost the caller every scalar beside it.

- [ ] **Step 2: Write the drain**

```cpp
llvm::Expected<PatchDrainResult> FunctionPatchManager::Drain() {
  PatchDrainResult Result;
  if (m_ring_address == LLDB_INVALID_ADDRESS)
    return Result;

  Process *Proc = m_target.GetProcessSP().get();
  if (!Proc || Proc->GetState() != lldb::eStateStopped)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   ToString(PatchFailure::NoProcess));
  ...
}
```

The body, in order: read `kPatchRingHeaderSize` bytes into a `PatchRingHeader`; read the ring's bytes; call `DrainPatchRing`; write `NewDrained` back into the header's `Drained` field; read each site's slot for its counters; and for each record build a value with
`ValueObjectConstResult::Create(Proc, RecordedType, ConstString(name), DataExtractor(&Rec.Value, ByteSize, ByteOrder, AddrSize))`
using the type recorded in step 1 and the byte size from that type — not eight, or a `char` capture would read seven bytes of zero padding beside it.

Reading only `ByteSize` bytes of a value the inferior zero-extended into eight is what makes the same record layout carry every scalar width.

- [ ] **Step 3: Drain wherever the process is already stopped**

Three places, per the spec, and none of them is a poll:

1. The drain trap's own site callback: drain, then resume without reporting a hit. It fires with nothing to drain more often than not — threads keep running until the stop is delivered, and the trap sits outside the per-site guards — so an empty drain is the ordinary case and must not be logged as a problem.
2. Any stop the consumer already takes. `Drain()` is idempotent, so calling it more often than necessary costs a memory read.
3. Process exit. Set an internal breakpoint on `exit` and `_exit` when the first capture site is installed, and drain from it. If neither symbol resolves, report the undrained count instead of dropping it — a run whose captures never reached the debugger must say so rather than report an empty list.

- [ ] **Step 4: Make the width handling testable, and test it**

Installing a capture-only site needs no public API and this prototype should not add one to the SB API, so the end-to-end coverage of captures lives in Task 11's MCP test. What *can* be covered here is the part most likely to be wrong: turning a record's eight bytes back into a value of the capture's own width.

Do not test that by reading `PatchRecord::Value` in a test and asserting on it — that asserts nothing about this code. Extract the width handling into a pure helper and test the helper. Add to `FunctionPatch.h`:

```cpp
/// The bytes of a recorded capture, at the width its type says it has.
///
/// The inferior copies a value into a fixed eight-byte field, so a narrower
/// scalar arrives zero-extended. Reading all eight bytes back would report the
/// padding beside a `char` as part of its value, and a byte order that differed
/// from the inferior's would reorder every multi-byte scalar.
llvm::SmallVector<uint8_t, 8> CaptureValueBytes(uint64_t Value, size_t ByteSize,
                                               lldb::ByteOrder Order);
```

and to `FunctionPatch.cpp`:

```cpp
llvm::SmallVector<uint8_t, 8>
lldb_private::CaptureValueBytes(uint64_t Value, size_t ByteSize,
                                lldb::ByteOrder Order) {
  const size_t Width = std::min<size_t>(ByteSize, sizeof(uint64_t));
  llvm::SmallVector<uint8_t, 8> Bytes(Width, 0);
  for (size_t I = 0; I < Width; ++I) {
    const uint8_t Byte = static_cast<uint8_t>(Value >> (8 * I));
    Bytes[Order == lldb::eByteOrderBig ? Width - 1 - I : I] = Byte;
  }
  return Bytes;
}
```

Then the tests in `FunctionPatchTest.cpp` assert real behaviour:

```cpp
TEST(FunctionPatchTest, ReadsANarrowCaptureWithoutItsPadding) {
  // A one-byte capture arrives zero-extended into eight. Reading all eight
  // would report seven bytes of padding as part of the value.
  auto Bytes = CaptureValueBytes(0xFF, 1, lldb::eByteOrderLittle);
  ASSERT_EQ(1u, Bytes.size());
  EXPECT_EQ(0xFF, Bytes[0]);
}

TEST(FunctionPatchTest, ReadsAFourByteCaptureWithoutTheHighHalf) {
  auto Bytes = CaptureValueBytes(0x00000000AABBCCDD, 4,
                                 lldb::eByteOrderLittle);
  ASSERT_EQ(4u, Bytes.size());
  EXPECT_EQ(0xDD, Bytes[0]);
  EXPECT_EQ(0xAA, Bytes[3]);
}

// A double reaches the record through a memcpy rather than a cast, so its bits
// are the bits the program held. They have to survive the trip back too.
TEST(FunctionPatchTest, RoundTripsADoublesBits) {
  const double Original = -1.5e-300;
  uint64_t Raw = 0;
  std::memcpy(&Raw, &Original, sizeof Raw);
  auto Bytes = CaptureValueBytes(Raw, sizeof(double), lldb::eByteOrderLittle);
  ASSERT_EQ(8u, Bytes.size());
  double Back = 0;
  std::memcpy(&Back, Bytes.data(), sizeof Back);
  EXPECT_EQ(Original, Back);
}

TEST(FunctionPatchTest, OrdersBytesForABigEndianReader) {
  auto Bytes = CaptureValueBytes(0x0000000000ABCDEF, 4, lldb::eByteOrderBig);
  ASSERT_EQ(4u, Bytes.size());
  EXPECT_EQ(0x00, Bytes[0]);
  EXPECT_EQ(0xEF, Bytes[3]);
}

// A type wider than the field cannot have fitted through it, so clamping is
// what keeps a bad type from reading past the record.
TEST(FunctionPatchTest, ClampsAWidthWiderThanTheField) {
  auto Bytes = CaptureValueBytes(~0ull, 16, lldb::eByteOrderLittle);
  EXPECT_EQ(8u, Bytes.size());
}
```

Step 2's `ValueObjectConstResult::Create` call then builds its `DataExtractor` from `CaptureValueBytes(Rec.Value, ByteSize, Order)` rather than from `&Rec.Value` directly.

- [ ] **Step 5: Verify against a real process**

Extend the probe harness from Task 6 to install a capture-only site through the C++ API by way of a temporary `lldb` command, or drive it from Task 11's MCP test. Confirm three things:

1. A run with 100,000 captured values completes with a bounded number of stops — one per ring fill, so roughly 100000 / 3072, not 100000.
2. A `double` capture reads back with its bits intact, which is what the memcpy rather than a cast is for. Add a `double` local to `main.c`'s `accumulate` and capture it.
3. A run with ten captured values reports all ten, which exercises the exit-path drain rather than the high-water one.

- [ ] **Step 6: Commit**

```bash
git add lldb/include/lldb/Target/FunctionPatch.h \
        lldb/source/Target/FunctionPatch.cpp \
        lldb/unittests/Target/FunctionPatchTest.cpp
git commit -m "[lldb] Let a captured value leave the process without stopping it

A value read at a stop costs a stop per hit. Written to a ring the injected
code owns, it costs a stop per ring fill, and the type it is read back as comes
from the copy's own debug info so the reported value is the value the program
held."
```

---

### Task 11: Make lldb-mcp use it by default

The goal that motivated the feature: an `observe` plan's conditions and scalar captures run in-process without being asked for, and the report says which mode each observation got.

**Files:**
- Modify: `lldb/source/Plugins/Protocol/MCP/ObservationEngine.h` (per-observation mode and reason)
- Modify: `lldb/source/Plugins/Protocol/MCP/ObservationEngine.cpp` (install, drain, report)
- Modify: `lldb/source/Plugins/Protocol/MCP/ObservationPlan.h` (the `Fast` escape hatch)
- Modify: `lldb/source/Plugins/Protocol/MCP/ObservationPlan.cpp` (parse it)
- Modify: `lldb/source/Protocol/MCP/ObserveSurface.cpp` (schema for `fast`)
- Modify: `lldb/test/API/tools/lldb-mcp/observe/TestObserve.py`
- Modify: `lldb/unittests/Protocol/ObservationPlanTest.cpp`

**Interfaces:**
- Consumes: `FunctionPatchManager::Install`, `Drain`, `SetGate`, `PatchRequest`, `PatchFailure`, `ToString` (Tasks 7, 9, 10).
- Produces: `ObservationPlan::Fast` (a `bool`, default true), and an `eval` field per observation in the report.

- [ ] **Step 1: Add the escape hatch to the plan**

In `ObservationPlan.h`, on `ObservationPlan`:

```cpp
  /// Whether a tracepoint's own work may be compiled into the program rather
  /// than done at a stop. On by default, because a condition evaluated at a
  /// stop is what makes a hot tracepoint unaffordable.
  ///
  /// Worth turning off when the program's own timing is the thing under
  /// investigation: a patched function is recompiled without optimization, so
  /// it is slower than the one it replaces and, where the original relied on
  /// what the optimizer did, can behave differently.
  bool Fast = true;
```

Parse it in `ParseObservationPlan` alongside the other booleans, and add it to the schema in `ObserveSurface.cpp` with a description in the same clipped register as its neighbours. Add a parser unit test in `ObservationPlanTest.cpp` asserting it defaults true and reads false.

- [ ] **Step 2: Install a patch per observation**

Where the engine sets up each observation's breakpoint and callbacks, and before it installs the stop-based ones, try the in-process route when `Plan.Fast` is set. One `PatchRequest` per observation:

- `Condition` from `Observation::WhenExpr`.
- `Captures` from `Observation::Capture`, minus any the manager refuses as non-scalar.
- `SkipFirst` and `OnlyHit` straight across.
- `Gated` set when the observation has a `CalledFrom` or an `EnabledAfter`; the engine's existing gate callbacks call `SetGate` instead of enabling and disabling a breakpoint.
- `WantStop` false when the observation has no condition and every capture was accepted — nothing then needs the debugger, so nothing should stop.
- `OnTrap` forwarding to the same `ObservationSite::OnHit` the stop-based path uses, so a hit is recorded identically however it was detected.

Refuse the whole in-process route, per observation, for `OnReturn` and for any `Emit` mode other than `EveryHit` (`PatchFailure::Unsupported`). Those fall back with no loss of correctness.

- [ ] **Step 3: Report which mode each observation got**

One field per observation, because `ObserveSurface.cpp` accounts for schema bytes deliberately:

```cpp
    // Said per observation because the answer differs per observation: one
    // tracepoint's condition may be compiled in while another's function had no
    // source to recompile. A caller comparing hit counts between runs needs to
    // know which of them paid for a stop per hit.
    O["eval"] = InProcess ? "in-process"
                          : ("stopped: " + FallbackReason).str();
```

And one run-level note naming the functions that were recompiled, because the program under test is running unoptimized copies of them and that is a property of the run rather than of any one observation.

- [ ] **Step 4: Drain at every stop the engine takes**

The engine already stops for conditions, gates, crashes and the timeout ceiling. Call `Drain()` at each, merge the captured values into the same per-hit records the stop-based path produces, and add `Lost` to the report when it is non-zero. Install the exit-path breakpoint from Task 10 when the first capture site goes in.

- [ ] **Step 5: Write the failing test**

Add to `TestObserve.py`, following the existing helpers in that file for building a plan and reading the report:

```python
    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_condition_runs_in_process_by_default(self):
        """A plan's condition is compiled into the program unless refused."""
        report = self.observe({
            "program": self.getBuildArtifact("a.out"),
            "observations": [{
                "at": "main.c:42",
                "when": "argc > 1000",
            }],
        })
        observation = report["observations"][0]
        self.assertEqual(observation["eval"], "in-process")
        self.assertEqual(observation["hits"], 0)

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_fast_false_uses_the_stopping_path(self):
        """The escape hatch removes the variable for anyone who needs it."""
        report = self.observe({
            "program": self.getBuildArtifact("a.out"),
            "fast": False,
            "observations": [{"at": "main.c:42", "when": "argc > 1000"}],
        })
        self.assertTrue(report["observations"][0]["eval"].startswith("stopped"))

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_scalar_captures_arrive_without_a_stop_per_hit(self):
        """Captures recorded in-process still reach the report, with types."""
        report = self.observe({
            "program": self.getBuildArtifact("a.out"),
            "observations": [{"at": "accumulate", "capture": ["seed", "rounds"]}],
        })
        observation = report["observations"][0]
        self.assertEqual(observation["eval"], "in-process")
        self.assertGreater(observation["hits"], 0)

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_on_return_falls_back_and_says_so(self):
        """A refusal names itself rather than being silent."""
        report = self.observe({
            "program": self.getBuildArtifact("a.out"),
            "observations": [{"at": "accumulate", "on_return": True,
                              "capture": ["seed"]}],
        })
        self.assertTrue(report["observations"][0]["eval"].startswith("stopped"))
```

Adapt `self.observe(...)` and the line number in `main.c:42` to whatever the existing test file uses — read it first; it has a full MCP client harness already.

- [ ] **Step 6: Run**

```bash
ninja -C /Users/work/Developer/llvm/build lldb ProtocolTests && \
  /Users/work/Developer/llvm/build/tools/lldb/unittests/Protocol/ProtocolTests && \
  /Users/work/Developer/llvm/build/bin/lldb-dotest -p TestObserve.py
```

Expected: the new tests pass and every existing `TestObserve.py` test still passes. The existing suite is the real gate here ��� a report whose hit counts or captured values changed shape because they came from a ring rather than a stop would break it, and that is exactly what it should do.

- [ ] **Step 7: Commit**

```bash
git add lldb/source/Plugins/Protocol/MCP/ObservationEngine.h \
        lldb/source/Plugins/Protocol/MCP/ObservationEngine.cpp \
        lldb/source/Plugins/Protocol/MCP/ObservationPlan.h \
        lldb/source/Plugins/Protocol/MCP/ObservationPlan.cpp \
        lldb/source/Protocol/MCP/ObserveSurface.cpp \
        lldb/unittests/Protocol/ObservationPlanTest.cpp \
        lldb/test/API/tools/lldb-mcp/observe/TestObserve.py
git commit -m "[lldb-mcp] Do a tracepoint's own work in the program, and say when it could not

Expression evaluation is both how a plan decides where to stop and how it
gathers what it reports, so a stop per hit is what bounds how much a plan can
watch. Which mode an observation got is said per observation, because a caller
comparing hit counts across runs needs to know which of them paid for it."
```

---

## Self-Review

**Spec coverage.** Every section of the spec maps to a task: the trampoline to 1, body extraction to 2, the control block to 3, the generated source to 4, `eProgramTrap` to 5, the JIT module gap to 6, the manager and the source-skew refusal to 7, the opt-in setting and breakpoint wiring to 8, re-patching and retirement to 9, captures and the drain policy to 10, the MCP surface and the report fields to 11.

Two spec items deliberately have no task, and both are documentation rather than code: the "known behavioural differences" list, which belongs in the setting's description (Task 8 step 1 carries the timing and semantics warning) and in the run-level note (Task 11 step 3); and the naming decision, which Task 7 step 9 implements by looking the symbol up in the JIT module.

**Where the plan is honestly weak.** Task 10 unit-tests the width arithmetic through a pure helper, which is real coverage, but it cannot unit-test the interaction between a live JIT, debug info and a ring — no unit test reaches that. Its coverage is Task 11's end-to-end test plus the manual verification in its step 5. Task 5 has the same shape and names the boundary explicitly. If either task's implementer finds a cheap way to unit-test what is deferred, taking it is an improvement, not a deviation.

**Task 8 step 6 leaves a choice open** between forwarding the trap site's callback to the user's `BreakpointLocation` and giving the site its own internal breakpoint. That is deliberate: which one is cleaner depends on how `StopInfoBreakpoint` handles a site whose constituent location sits at a different address, which is worth discovering with the code in hand rather than guessing here. The task says to record which was chosen.

**Interface consistency.** `PatchRequest` is the name used in Tasks 7, 8 and 11; `PatchInjection` is the builder's own type in Task 4 and the manager translates between them. `CaptureLocalName` is defined in Task 4 and consumed in Task 10 with the same spelling. `PatchFailure` and `ToString` are defined in Task 7 and consumed in 10 and 11. The spec's interface sketch named the manager's methods `Install`, `Remove`, `Drain` and `SetGate`, and the plan uses those four names throughout; the sketch's `Injection` became `PatchRequest` to keep it distinct from the builder's `PatchInjection`, and `DrainResult` became `PatchDrainResult` for the same reason.
