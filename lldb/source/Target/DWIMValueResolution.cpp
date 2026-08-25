//===-- DWIMValueResolution.cpp -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/DWIMValueResolution.h"
#include "lldb/Expression/ExpressionVariable.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/ValueObject/ValueObject.h"

using namespace lldb_private;

llvm::StringRef lldb_private::ToString(ValueResolutionTier Tier) {
  switch (Tier) {
  case ValueResolutionTier::VariablePath:
    return "path";
  case ValueResolutionTier::PersistentVariable:
    return "persistent";
  case ValueResolutionTier::Expression:
    return "expression";
  case ValueResolutionTier::Unresolved:
    return "unavailable";
  }
  return "unavailable";
}

bool lldb_private::IsVariablePathEligible(llvm::StringRef Expr,
                                          bool AllowPointerPaths) {
  if (Expr.empty())
    return false;

  // A call is never a path, and `*` and `&` are ambiguous with multiplication
  // and bitwise and, so both are excluded in either mode.
  if (Expr.find_first_of("*&()") != llvm::StringRef::npos)
    return false;

  // `.` is the only operator a path may contain by default, because `->` and
  // `[]` can be overloaded in C++ and a path parser that accepted them would
  // disagree with the compiler.
  if (!AllowPointerPaths)
    return Expr.find_first_of("->[]") == llvm::StringRef::npos;

  // With pointer paths admitted, `->` and `[]` join `.`, but nothing else
  // does.
  if (Expr.find_first_of("+/%<!=|^~?:,;{}\"' \t\n\v\f\r") !=
      llvm::StringRef::npos)
    return false;

  // `-` and `>` are only admissible as the two halves of `->`; on their own
  // they are subtraction and comparison.
  for (size_t I = 0, E = Expr.size(); I != E; ++I) {
    if (Expr[I] == '-' && (I + 1 == E || Expr[I + 1] != '>'))
      return false;
    if (Expr[I] == '>' && (I == 0 || Expr[I - 1] != '-'))
      return false;
  }
  return true;
}

uint32_t
lldb_private::VariablePathOptions(const ValueResolutionOptions &Opts) {
  uint32_t PathOptions =
      StackFrame::eExpressionPathOptionsAllowDirectIVarAccess;

  // DIL reads this flag as "do not consult the compile unit's globals or the
  // module's global variable list", which is every route a bare identifier
  // naming a global or a file-scope static could be found by. With it set the
  // path tier cannot resolve such a name at all, so the name reaches the
  // expression evaluator and is compiled -- once per resolution, since nothing
  // about a path is cached between them.
  if (!Opts.AllowGlobals)
    PathOptions |= StackFrame::eExpressionPathOptionsDisallowGlobals;
  return PathOptions;
}

ValueResolution
lldb_private::ResolveValueDWIM(llvm::StringRef Expr, StackFrame *Frame,
                               Target &Tgt, ExecutionContextScope *Scope,
                               const ValueResolutionOptions &Opts) {
  ValueResolution Result;

  // First: a variable expression path. No compilation, no code runs in the
  // inferior, microseconds rather than milliseconds.
  if (Frame && IsVariablePathEligible(Expr, Opts.AllowPointerPaths)) {
    // The path parser has to be told to accept `->` and `[]` before the wider
    // predicate admits anything the simple mode could parse.
    lldb::DILMode Mode =
        Opts.AllowPointerPaths ? lldb::eDILModeLegacy : lldb::eDILModeSimple;

    lldb::VariableSP VarSP;
    Status PathStatus;
    lldb::ValueObjectSP ValObj = Frame->GetValueForVariableExpressionPath(
        Expr, Opts.UseDynamic, VariablePathOptions(Opts), VarSP, PathStatus,
        Mode);
    if (ValObj && PathStatus.Success() && ValObj->GetError().Success()) {
      if (!Opts.SuppressPersistentResult)
        if (lldb::ValueObjectSP Persisted = ValObj->Persist())
          ValObj = Persisted;
      Result.Value = ValObj;
      Result.Tier = ValueResolutionTier::VariablePath;
      return Result;
    }
  }

  // Second: a persistent variable.
  if (Opts.TryPersistent && Expr.starts_with("$")) {
    SourceLanguage Language = Opts.Language;
    if (!Language && Frame)
      Language = Frame->GuessLanguage();
    if (auto *State = Tgt.GetPersistentExpressionStateForLanguage(
            Language.AsLanguageType()))
      if (lldb::ExpressionVariableSP VarSP = State->GetVariable(Expr))
        if (lldb::ValueObjectSP ValObj = VarSP->GetValueObject()) {
          Result.Value = ValObj;
          Result.Tier = ValueResolutionTier::PersistentVariable;
          return Result;
        }
  }

  // Third: compile and evaluate.
  lldb::ValueObjectSP ValObj;
  lldb::ExpressionResults ExprResult = Tgt.EvaluateExpression(
      Expr, Scope, ValObj, Opts.ExprOptions, &Result.FixedExpression);
  Result.Value = ValObj;
  Result.ExprResult = ExprResult;

  // A completed evaluation can still leave an error on the value, so the
  // evaluator's verdict is what decides the tier.
  if (ExprResult == lldb::eExpressionCompleted) {
    Result.Tier = ValueResolutionTier::Expression;
  } else {
    Result.Tier = ValueResolutionTier::Unresolved;
    Result.Error = ValObj ? ValObj->GetError().Clone()
                          : Status::FromErrorString("expression failed");
  }
  return Result;
}
