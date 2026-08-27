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

namespace {

/// The prefixes every name the builder emits begins with, and lldb's own
/// throughout the expression machinery. Two spellings because the preamble's
/// macros are in capitals, as macros are.
constexpr llvm::StringLiteral kInjectedPrefixes[] = {
    llvm::StringLiteral("__lldb_"), llvm::StringLiteral("__LLDB_")};

/// \p Name, with \p Tag appended if it is not empty.
///
/// Every name the preamble declares goes through this, so that a non-empty tag
/// keeps two compiles' declarations from colliding and an empty one -- the
/// default -- reproduces the untagged spelling every existing caller expects.
std::string Tagged(llvm::StringRef Tag, llvm::StringRef Name) {
  if (Tag.empty())
    return Name.str();
  return (Name + "_" + Tag).str();
}

} // namespace

std::string lldb_private::CaptureLocalName(llvm::StringRef Tag,
                                           uint32_t SiteID, uint32_t Capture) {
  return Tagged(Tag,
               llvm::formatv("__lldb_cap_{0}_{1}", SiteID, Capture).str());
}

bool lldb_private::IsInjectedName(llvm::StringRef Name) {
  return llvm::any_of(kInjectedPrefixes, [Name](llvm::StringRef Prefix) {
    return Name.starts_with(Prefix);
  });
}

namespace {

/// The declarations every patch needs, ahead of the body.
///
/// Attributed to a file of its own so that none of it claims a line of the
/// program's source, which would make a stop in the preamble report a line the
/// user could read but that says nothing.
std::string Preamble(const PatchSourceRequest &Request) {
  const llvm::StringRef Tag = Request.Tag;
  const std::string RecT = Tagged(Tag, "__lldb_rec_t");
  const std::string HdrT = Tagged(Tag, "__lldb_hdr_t");
  const std::string SiteT = Tagged(Tag, "__lldb_site_t");
  const std::string HdrMacro = Tagged(Tag, "__LLDB_HDR");
  const std::string RecFn = Tagged(Tag, "__lldb_rec");

  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << "#line 1 \"<lldb patch preamble>\"\n";
  OS << llvm::formatv("struct {0} {{ unsigned int site, cap; unsigned long "
                      "long hit, val; };\n",
                      RecT);

  // The header's shape is a contract with the struct the debugger reads the
  // block with (PatchRingHeader in lldb/include/lldb/Target/PatchControlBlock.h,
  // kPatchRingHeaderSize), so it cannot depend on what any one patch happens to
  // need. Omitting a field would move `ring` and cost nothing, since a
  // declaration is not storage, but it would break the contract. Always emit the
  // full declaration.
  OS << llvm::formatv(
      "struct {0} {{ unsigned long seq, drained, capacity, high_water; "
      "struct {1} ring[]; };\n",
      HdrT, RecT);

  OS << llvm::formatv(
      "struct {0} {{ unsigned long hits, cond_true; unsigned char gate; "
      "};\n",
      SiteT);
  OS << llvm::formatv("#define {0} ((volatile struct {1} *){2:x+})\n",
                      HdrMacro, HdrT, Request.RingAddress);
  OS << llvm::formatv(
      "static void {0}(unsigned k, unsigned c, unsigned long long h, "
      "unsigned long long v) {{ "
      "unsigned long s = __atomic_fetch_add(&{1}->seq, 1, "
      "__ATOMIC_RELAXED); volatile struct {2} *r = "
      "&{1}->ring[s & {3}]; r->site = k; r->cap = c; r->hit = h; r->val = v; "
      "}\n",
      RecFn, HdrMacro, RecT,
      Request.RingCapacity ? Request.RingCapacity - 1 : 0);
  return Text;
}

/// One injection, as a single physical line.
///
/// Every injection opens with a declaration and closes with a use of what it
/// declared, both at the injection's own brace depth. That is not decoration: an
/// injection goes in ahead of a line without knowing whether that line is the
/// sole body of a brace-less `if`, `for`, `while`, `do` or `else`. If it were,
/// and the injection were one self-contained statement, it would become that
/// body and the statement it was injected ahead of would leave the control
/// structure -- a program that compiles and does something else, which is the one
/// outcome this may not produce. A declaration cannot be a brace-less body and
/// still have its name visible to what follows it, so this shape is refused by
/// the compiler instead, and the caller falls back to evaluating at a stop.
std::string InjectionLine(llvm::StringRef Tag, const PatchInjection &Inj) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);

  const std::string SiteT = Tagged(Tag, "__lldb_site_t");
  const std::string HdrMacro = Tagged(Tag, "__LLDB_HDR");
  const std::string RecFn = Tagged(Tag, "__lldb_rec");
  const std::string Slot =
      llvm::formatv("((volatile struct {0} *){1:x+})", SiteT,
                    Inj.SlotAddress)
          .str();
  const std::string Hit =
      Tagged(Tag, llvm::formatv("__lldb_h_{0}", Inj.SiteID).str());

  // Declared ahead of the gate rather than inside it, so that the declaration is
  // the first thing the injection is made of whether or not there is a gate.
  OS << llvm::formatv("unsigned long {0} = 0; ", Hit);

  if (Inj.Gated)
    OS << llvm::formatv("if ({0}->gate) {{ ", Slot);

  OS << llvm::formatv(
      "{0} = __atomic_add_fetch(&{1}->hits, 1, __ATOMIC_RELAXED); ", Hit, Slot);

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
    const std::string Local = CaptureLocalName(Tag, Inj.SiteID, I);
    const std::string Value =
        Tagged(Tag, llvm::formatv("__lldb_v_{0}_{1}", Inj.SiteID, I).str());
    // A memcpy rather than a cast, because a cast converts and a double's bits
    // would not survive it. The width comes from the value itself, so every
    // scalar is handled by the same line.
    OS << llvm::formatv("__typeof__({0}) {1} = ({0}); ", Inj.Captures[I],
                        Local);
    OS << llvm::formatv("unsigned long long {0} = 0; ", Value);
    OS << llvm::formatv("__builtin_memcpy(&{0}, &{1}, sizeof {1}); ", Value,
                        Local);
    // The hit the site's own counter numbered this one travels with the value,
    // because a value is only a value of something once it is known which hit
    // it belongs to -- and the records of two threads inside this site arrive
    // interleaved.
    OS << llvm::formatv("{0}({1}, {2}, {3}, {4}); ", RecFn, Inj.SiteID, I, Hit,
                        Value);
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
    OS << llvm::formatv("if ({0}->seq - {0}->drained >= "
                        "{0}->high_water) __builtin_debugtrap(); ",
                        HdrMacro);

  if (Inj.Gated)
    OS << "} ";

  // The use that has to be in scope wherever the declaration is, which is what
  // makes an injection that landed as a brace-less body a compile error rather
  // than a program that quietly does something else.
  OS << llvm::formatv("(void){0};", Hit);

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
      OS << InjectionLine(Request.Tag, *Ordered[Next]) << "\n";
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
