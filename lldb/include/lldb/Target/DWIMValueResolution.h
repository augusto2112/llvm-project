//===-- DWIMValueResolution.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_TARGET_DWIMVALUERESOLUTION_H
#define LLDB_TARGET_DWIMVALUERESOLUTION_H

#include "lldb/Target/Target.h"
#include "lldb/Utility/Status.h"
#include "lldb/lldb-enumerations.h"
#include "lldb/lldb-forward.h"
#include "lldb/lldb-private-types.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <optional>
#include <string>

namespace lldb_private {

/// Which mechanism produced a value. Callers surface this so that the cost of
/// a capture is visible: a variable path is a debug-info lookup and a memory
/// read, while an expression compiles and runs code in the inferior.
enum class ValueResolutionTier {
  VariablePath,
  PersistentVariable,
  Expression,
  Unresolved,
};

struct ValueResolutionOptions {
  /// Permit `->` and `[]` in variable paths. dwim-print leaves this off
  /// because those operators can be overloaded; a caller tracing a
  /// pointer-heavy codebase wants them on and reports the tier it got.
  bool AllowPointerPaths = false;

  /// Let a bare identifier name a global or a file-scope static at the path
  /// tier. Off, a name that resolves to one is not found there at all and falls
  /// through to the expression evaluator: measured on a `static int` at file
  /// scope, 9.6 ms per resolution against 0.17 ms for a parameter of the same
  /// function, which is 2.6x the cost of an entire tracepoint hit. On, the same
  /// name is a debug-info lookup and a memory read like any other path.
  ///
  /// A local still wins: the interpreter looks the frame up first and reaches
  /// for a global only when that finds nothing, so this admits a name rather
  /// than reordering one. dwim-print leaves it off and is unaffected.
  bool AllowGlobals = false;

  /// Look up `$name` in the persistent expression state. Useful for a REPL,
  /// unwanted for a capture, where a stale `$3` must not shadow a variable.
  bool TryPersistent = true;

  /// Whether a value resolved as a variable path is left unnamed rather than
  /// bound to a fresh `$N`. Persisting costs a new entry in the target's
  /// persistent state per resolution, so a repeated resolution wants it off.
  bool SuppressPersistentResult = true;

  /// eDynamicDontRunTarget resolves C++ vtables and metadata-readable
  /// dynamic types without executing code in the inferior. Do not raise this
  /// to eDynamicCanRunTarget on a repeated trigger: for Objective-C and Swift
  /// it can mean a runtime call per hit.
  lldb::DynamicValueType UseDynamic = lldb::eDynamicDontRunTarget;

  /// Selects the persistent expression state that `$name` is looked up in.
  /// When unset the language is guessed from the frame, so a caller that has
  /// already settled on a language must pass it here or that choice is lost.
  SourceLanguage Language;

  EvaluateExpressionOptions ExprOptions;
};

struct ValueResolution {
  lldb::ValueObjectSP Value;
  ValueResolutionTier Tier = ValueResolutionTier::Unresolved;

  /// Why resolution failed. Only meaningful for the Unresolved tier; a
  /// resolved value carries its own error, which for a void expression is set
  /// even though resolution succeeded.
  Status Error;

  std::string FixedExpression;

  /// The evaluator's verdict, present only when the expression tier ran. A
  /// completed evaluation can still leave an error on \ref Value, so this is
  /// not recoverable from the value alone.
  std::optional<lldb::ExpressionResults> ExprResult;
};

/// Whether \p Expr can be resolved as a variable expression path rather than
/// by the expression evaluator. Only `.` joins path components unless
/// \p AllowPointerPaths is set, which additionally admits `->` and `[]`.
bool IsVariablePathEligible(llvm::StringRef Expr, bool AllowPointerPaths);

/// The frame expression-path option bits \ref ResolveValueDWIM asks the path
/// tier for, given \p Opts.
///
/// Split out because this mapping is the whole of what
/// \ref ValueResolutionOptions::AllowGlobals does, and because the difference
/// it makes -- a memory read or a compiled expression, per resolution -- is
/// otherwise only visible from a running inferior stopped in a frame whose
/// compile unit has a global in it.
uint32_t VariablePathOptions(const ValueResolutionOptions &Opts);

/// Resolve \p Expr to a value, trying the cheapest mechanism that can work:
/// a variable expression path, then a persistent variable, then the expression
/// evaluator. \p Frame may be null, in which case the path tier is skipped.
ValueResolution ResolveValueDWIM(llvm::StringRef Expr, StackFrame *Frame,
                                 Target &Tgt, ExecutionContextScope *Scope,
                                 const ValueResolutionOptions &Opts);

llvm::StringRef ToString(ValueResolutionTier Tier);

} // namespace lldb_private

#endif // LLDB_TARGET_DWIMVALUERESOLUTION_H
