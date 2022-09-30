//===-- SwiftUserExpression.cpp ---------------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include <stdio.h>
#if HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#include "SwiftASTManipulator.h"
#include "SwiftExpressionParser.h"
#include "SwiftREPLMaterializer.h"
#include "SwiftExpressionSourceCode.h"

#include "Plugins/LanguageRuntime/Swift/SwiftLanguageRuntime.h"
#include "lldb/Core/Module.h"
#include "lldb/Expression/DiagnosticManager.h"
#include "lldb/Expression/ExpressionParser.h"
#include "lldb/Expression/ExpressionSourceCode.h"
#include "lldb/Expression/IRExecutionUnit.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/Symbol.h"
#include "lldb/Symbol/Type.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Utility/LLDBAssert.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Timer.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/Type.h"
#include "swift/AST/Types.h"

#include <cstdlib>
#include <map>
#include <string>

#include "SwiftUserExpression.h"

using namespace lldb_private;

char SwiftUserExpression::ID;

SwiftUserExpression::SwiftUserExpression(
    ExecutionContextScope &exe_scope, llvm::StringRef expr,
    llvm::StringRef prefix, lldb::LanguageType language,
    ResultType desired_type, const EvaluateExpressionOptions &options)
    : LLVMUserExpression(exe_scope, expr, prefix, language, desired_type,
                         options),
      m_type_system_helper(*m_target_wp.lock().get()),
      m_result_delegate(exe_scope.CalculateTarget(), *this, false),
      m_error_delegate(exe_scope.CalculateTarget(), *this, true),
      m_persistent_variable_delegate(*this) {
  m_runs_in_playground_or_repl =
      options.GetREPLEnabled() || options.GetPlaygroundTransformEnabled();
}

SwiftUserExpression::~SwiftUserExpression() {}

void SwiftUserExpression::WillStartExecuting() {
  if (auto process = m_jit_process_wp.lock()) {
    if (auto *swift_runtime = SwiftLanguageRuntime::Get(process))
      swift_runtime->WillStartExecutingUserExpression(
          m_runs_in_playground_or_repl);
    else
      llvm_unreachable("Can't execute a swift expression without a runtime");
  } else
    llvm_unreachable("Can't execute an expression without a process");
}

void SwiftUserExpression::DidFinishExecuting() {
  if (auto process = m_jit_process_wp.lock()) {
    if (auto *swift_runtime = SwiftLanguageRuntime::Get(process))
      swift_runtime->DidFinishExecutingUserExpression(
          m_runs_in_playground_or_repl);
    else
      llvm_unreachable("Can't execute a swift expression without a runtime");
  }
}

/// Determine whether we have a Swift language symbol context. This handles
/// some special cases, such as when the expression language is unknown, or
/// when we have to guess from a mangled name.
static bool isSwiftLanguageSymbolContext(const SwiftUserExpression &expr,
                                         const SymbolContext &sym_ctx) {
  if (sym_ctx.comp_unit && (expr.Language() == lldb::eLanguageTypeUnknown ||
                            expr.Language() == lldb::eLanguageTypeSwift)) {
    if (sym_ctx.comp_unit->GetLanguage() == lldb::eLanguageTypeSwift)
      return true;
  } else if (sym_ctx.symbol && expr.Language() == lldb::eLanguageTypeUnknown) {
    if (sym_ctx.symbol->GetMangled().GuessLanguage() ==
        lldb::eLanguageTypeSwift)
      return true;
  }
  return false;
}

/// Information about `self` in a frame.
struct SwiftSelfInfo {
  /// Whether `self` is a metatype (i.e. whether we're in a static method).
  bool is_metatype = false;

  /// Adjusted type of `self`. If we're in a static method, this is an instance
  /// type.
  CompilerType type = {};

  /// Underlying Swift type for the adjusted type of `self`.
  swift::TypeBase *swift_type = nullptr;

  /// Type flags for the adjusted type of `self`.
  Flags type_flags = {};
};

/// Find information about `self` in the frame.
static llvm::Optional<SwiftSelfInfo>
findSwiftSelf(StackFrame &frame, lldb::VariableSP self_var_sp) {
  SwiftSelfInfo info;

  lldb::ValueObjectSP valobj_sp = frame.GetValueObjectForFrameVariable(
      self_var_sp, lldb::eDynamicDontRunTarget);

  // 1) Try finding the type of `self` from its ValueObject.
  if (valobj_sp && valobj_sp->GetError().Success())
    info.type = valobj_sp->GetCompilerType();

  // 2) If (1) fails, try finding the type of `self` from its Variable.
  if (!info.type.IsValid())
    if (Type *self_lldb_type = self_var_sp->GetType())
      info.type = self_var_sp->GetType()->GetForwardCompilerType();

  // 3) If (1) and (2) fail, give up.
  if (!info.type.IsValid())
    return llvm::None;

  // 4) If `self` is a metatype, get its instance type.
  if (Flags(info.type.GetTypeInfo())
          .AllSet(lldb::eTypeIsSwift | lldb::eTypeIsMetatype)) {
    info.type = TypeSystemSwift::GetInstanceType(info.type);
    info.is_metatype = true;
  }

  info.swift_type = GetSwiftType(info.type).getPointer();
  if (auto *dyn_self =
          llvm::dyn_cast_or_null<swift::DynamicSelfType>(info.swift_type))
    info.swift_type = dyn_self->getSelfType().getPointer();

  // 5) If the adjusted type isn't equal to the type according to the runtime,
  // switch it to the latter type.
  if (info.swift_type && (info.swift_type != info.type.GetOpaqueQualType()))
    info.type = ToCompilerType(info.swift_type);

  info.type_flags = Flags(info.type.GetTypeInfo());

  if (!info.type.IsValid())
    return llvm::None;
  return info;
}

void SwiftUserExpression::ScanContext(ExecutionContext &exe_ctx, Status &err) {
  Log *log = GetLog(LLDBLog::Expressions);
  LLDB_LOG(log, "SwiftUserExpression::ScanContext()");
  LLDB_SCOPED_TIMER();

  m_target = exe_ctx.GetTargetPtr();
  if (!m_target) {
    LLDB_LOG(log, "  [SUE::SC] Null target");
    return;
  }

  StackFrame *frame = exe_ctx.GetFramePtr();
  if (!frame) {
    LLDB_LOG(log, "  [SUE::SC] Null stack frame");
    return;
  }

  SymbolContext sym_ctx = frame->GetSymbolContext(
      lldb::eSymbolContextFunction | lldb::eSymbolContextBlock |
      lldb::eSymbolContextCompUnit | lldb::eSymbolContextSymbol);
  bool frame_is_swift = isSwiftLanguageSymbolContext(*this, sym_ctx);
  if (!frame_is_swift) {
    LLDB_LOG(log, "  [SUE::SC] Frame is not swift-y");
    return;
  }

  if (!m_swift_ast_ctx) {
    LLDB_LOG(log, "  [SUE::SC] NULL Swift AST Context");
    return;
  }
  if (!m_swift_ast_ctx->GetClangImporter()) {
    LLDB_LOG(log, "  [SUE::SC] Swift AST Context has no Clang importer");
    return;
  }

  if (m_swift_ast_ctx->HasFatalErrors()) {
    LLDB_LOG(log, "  [SUE::SC] Swift AST Context has fatal errors");
    return;
  }

  LLDB_LOG(log, "  [SUE::SC] Compilation unit is swift");

  Block *function_block = sym_ctx.GetFunctionBlock();
  if (!function_block) {
    LLDB_LOG(log, "  [SUE::SC] No function block");
    return;
  }

  lldb::VariableListSP variable_list_sp(
      function_block->GetBlockVariableList(true));
  if (!variable_list_sp) {
    LLDB_LOG(log, "  [SUE::SC] No block variable list");
    return;
  }

  lldb::VariableSP self_var_sp(
      variable_list_sp->FindVariable(ConstString("self")));
  if (!self_var_sp || !SwiftLanguageRuntime::IsSelf(*self_var_sp)) {
    LLDB_LOG(log, "  [SUE::SC] No valid `self` variable");
    return;
  }

  // If we have a self variable, but it has no location at the current PC, then
  // we can't use it.  Set the self var back to empty and we'll just pretend we
  // are in a regular frame, which is really the best we can do.
  if (!self_var_sp->LocationIsValidForFrame(frame)) {
    LLDB_LOG(log, "  [SUE::SC] `self` variable location not valid for frame");
    return;
  }

  auto maybe_self_info = findSwiftSelf(*frame, self_var_sp);
  if (!maybe_self_info) {
    LLDB_LOG(log, "  [SUE::SC] Could not determine info about `self`");
    return;
  }

  // Check to see if we are in a class func of a class (or static func of a
  // struct) and adjust our type to point to the instance type.
  SwiftSelfInfo info = *maybe_self_info;

  m_in_static_method = info.is_metatype;

  if (info.type_flags.AllSet(lldb::eTypeIsSwift | lldb::eTypeInstanceIsPointer))
    m_is_class |= info.type_flags.Test(lldb::eTypeIsClass);

  // Handle weak self.
  auto *ref_type =
      llvm::dyn_cast_or_null<swift::ReferenceStorageType>(info.swift_type);
  if (ref_type && ref_type->getOwnership() == swift::ReferenceOwnership::Weak) {
    m_is_class = true;
    m_is_weak_self = true;
  }

  m_needs_object_ptr = !m_in_static_method;

  LLDB_LOGF(log, "  [SUE::SC] Containing class name: %s",
            info.type.GetTypeName().AsCString());
}

static SwiftPersistentExpressionState *
GetPersistentState(Target *target, ExecutionContext &exe_ctx) {
  auto exe_scope = exe_ctx.GetBestExecutionContextScope();
  if (!exe_scope)
    return nullptr;
  return target->GetSwiftPersistentExpressionState(*exe_scope);
}


/// Returns the Swift type for a ValueObject representing a variable.
/// An invalid CompilerType is returned on error.
static CompilerType GetSwiftTypeForVariableValueObject(
    lldb::ValueObjectSP valobj_sp, lldb::StackFrameSP &stack_frame_sp,
    SwiftLanguageRuntime *runtime) {
  LLDB_SCOPED_TIMER();
  // Check that the passed ValueObject is valid.
  if (!valobj_sp || valobj_sp->GetError().Fail())
    return {};
  CompilerType result = valobj_sp->GetCompilerType();
  if (!result)
    return {};
  /* result = runtime->BindGenericTypeParameters(*stack_frame_sp, result); */
  /* if (!result) */
  /*   return {}; */
  if (!result.GetTypeSystem()->SupportsLanguage(lldb::eLanguageTypeSwift))
    return {};
  return result;
}

/// Return the type for a local variable. This function is threading a
/// fine line between using dynamic type resolution to resolve generic
/// types and not resolving too much: Objective-C classes can have
/// more specific private implementations that LLDB can resolve, but
/// SwiftASTContext cannot see because there is no header file that
/// would declare them.
static CompilerType ResolveVariable(
    lldb::VariableSP variable_sp, lldb::StackFrameSP &stack_frame_sp,
    SwiftLanguageRuntime * runtime, lldb::DynamicValueType use_dynamic) {
  LLDB_SCOPED_TIMER();
  lldb::ValueObjectSP valobj_sp =
      stack_frame_sp->GetValueObjectForFrameVariable(variable_sp,
                                                     lldb::eNoDynamicValues);
  const bool use_dynamic_value = use_dynamic > lldb::eNoDynamicValues;

  CompilerType var_type =
      GetSwiftTypeForVariableValueObject(valobj_sp, stack_frame_sp, runtime);

  if (!var_type.IsValid())
    return {};

  // If the type can't be realized and dynamic types are allowed, fall back to
  // the dynamic type.
  if (!SwiftASTContext::IsFullyRealized(var_type) && use_dynamic_value) {
    var_type = GetSwiftTypeForVariableValueObject(
        valobj_sp->GetDynamicValue(use_dynamic), stack_frame_sp, runtime);
    if (!var_type.IsValid())
      return {};
  }
  return var_type;
}
/// Create a \c VariableInfo record for \c variable if there isn't
/// already shadowing inner declaration in \c processed_variables.
static llvm::Optional<llvm::Error> AddVariableInfo(
    lldb::VariableSP variable_sp, lldb::StackFrameSP &stack_frame_sp,
    SwiftASTContextForExpressions &ast_context,
    SwiftLanguageRuntime *runtime,
    llvm::SmallDenseSet<const char *, 8> &processed_variables,
    llvm::SmallVectorImpl<SwiftASTManipulator::VariableInfo> &local_variables,
    lldb::DynamicValueType use_dynamic) {
  LLDB_SCOPED_TIMER();

  llvm::StringRef name = variable_sp->GetUnqualifiedName().GetStringRef();
  const char *name_cstr = name.data();
  assert(llvm::StringRef(name_cstr) == name && "missing null terminator");
  if (name.empty())
    return {};

  // To support "guard let self = self" the function argument "self"
  // is processed (as the special self argument) even if it is
  // shadowed by a local variable.
  bool is_self = SwiftLanguageRuntime::IsSelf(*variable_sp);
  const char *overridden_name = name_cstr;
  if (is_self)
    overridden_name = "$__lldb_injected_self";

  if (processed_variables.count(overridden_name))
    return {};

  if (!stack_frame_sp)
    return llvm::None;

  CompilerType var_type =
      ResolveVariable(variable_sp, stack_frame_sp, runtime, use_dynamic);

  Status error;
  CompilerType target_type = ast_context.ImportType(var_type, error);

  // If the import failed, give up.
  if (!target_type.IsValid())
    return {};

  // If we couldn't fully realize the type, then we aren't going
  // to get very far making a local out of it, so discard it here.
  Log *log = GetLog(LLDBLog::Types | LLDBLog::Expressions);
  if (!SwiftASTContext::IsFullyRealized(target_type)) {
    if (log)
      log->Printf("Discarding local %s because we couldn't fully realize it, "
                  "our best attempt was: %s.",
                  name_cstr, target_type.GetDisplayTypeName().AsCString("<unknown>"));
    // Not realizing self is a fatal error for an expression and the
    // Swift compiler error alone is not particularly useful.
    if (is_self)
      return llvm::make_error<llvm::StringError>(
          llvm::inconvertibleErrorCode(),
          "Couldn't realize Swift AST type of self. Hint: using `v` to "
          "directly inspect variables and fields may still work.");
    return {};
  }

  if (log && is_self)
    if (swift::Type swift_type = GetSwiftType(target_type)) {
      std::string s;
      llvm::raw_string_ostream ss(s);
      swift_type->dump(ss);
      ss.flush();
      log->Printf("Adding injected self: type (%p) context(%p) is: %s",
                  static_cast<void *>(swift_type.getPointer()),
                  static_cast<void *>(ast_context.GetASTContext()), s.c_str());
    }
  // A one-off clone of variable_sp with the type replaced by target_type.
  auto patched_variable_sp = std::make_shared<lldb_private::Variable>(
      0, variable_sp->GetName().GetCString(), "",
      std::make_shared<lldb_private::SymbolFileType>(
          *variable_sp->GetType()->GetSymbolFile(),
          std::make_shared<lldb_private::Type>(
              0, variable_sp->GetType()->GetSymbolFile(),
              variable_sp->GetType()->GetName(), llvm::None,
              variable_sp->GetType()->GetSymbolContextScope(), LLDB_INVALID_UID,
              Type::eEncodingIsUID, variable_sp->GetType()->GetDeclaration(),
              target_type, lldb_private::Type::ResolveState::Full,
              variable_sp->GetType()->GetPayload())),
      variable_sp->GetScope(), variable_sp->GetSymbolContextScope(),
      variable_sp->GetScopeRange(),
      const_cast<lldb_private::Declaration *>(&variable_sp->GetDeclaration()),
      variable_sp->LocationExpression(), variable_sp->IsExternal(),
      variable_sp->IsArtificial(),
      variable_sp->GetLocationIsConstantValueData(),
      variable_sp->IsStaticMember(), variable_sp->IsConstant());
  SwiftASTManipulatorBase::VariableMetadataSP metadata_sp(
      new SwiftASTManipulatorBase::VariableMetadataVariable(patched_variable_sp));
  SwiftASTManipulator::VariableInfo variable_info(
      target_type, ast_context.GetASTContext()->getIdentifier(overridden_name),
      metadata_sp,
      variable_sp->IsConstant() ? swift::VarDecl::Introducer::Let
                                : swift::VarDecl::Introducer::Var);

  local_variables.push_back(variable_info);
  processed_variables.insert(overridden_name);
  return {};
}

/// Create a \c VariableInfo record for each visible variable.
static llvm::Optional<llvm::Error> RegisterAllVariables(
    SymbolContext &sc, lldb::StackFrameSP &stack_frame_sp,
    SwiftASTContextForExpressions &ast_context,
    llvm::SmallVectorImpl<SwiftASTManipulator::VariableInfo> &local_variables,
    lldb::DynamicValueType use_dynamic) {
  LLDB_SCOPED_TIMER();
  if (!sc.block && !sc.function)
    return {};

  Block *block = sc.block;
  Block *top_block = block->GetContainingInlinedBlock();

  if (!top_block)
    top_block = &sc.function->GetBlock(true);

  SwiftLanguageRuntime *language_runtime = nullptr;

  if (stack_frame_sp)
    language_runtime =
        SwiftLanguageRuntime::Get(stack_frame_sp->GetThread()->GetProcess());

  // The module scoped variables are stored at the CompUnit level, so
  // after we go through the current context, then we have to take one
  // more pass through the variables in the CompUnit.
  VariableList variables;

  // Proceed from the innermost scope outwards, adding all variables
  // not already shadowed by an inner declaration.
  llvm::SmallDenseSet<const char *, 8> processed_names;
  bool done = false;
  do {
    // Iterate over all parent contexts *including* the top_block.
    if (block == top_block)
      done = true;
    bool can_create = true;
    bool get_parent_variables = false;
    bool stop_if_block_is_inlined_function = true;

    block->AppendVariables(
        can_create, get_parent_variables, stop_if_block_is_inlined_function,
        [](Variable *) { return true; }, &variables);

    if (!done)
      block = block->GetParent();
  } while (block && !done);

  // Also add local copies of globals. This is in many cases redundant
  // work because the globals would also be found in the expression
  // context's Swift module, but it allows a limited form of
  // expression evaluation to work even if the Swift module failed to
  // load, as long as the module isn't necessary to resolve the type
  // or aother symbols in the expression.
  if (sc.comp_unit) {
    lldb::VariableListSP globals_sp = sc.comp_unit->GetVariableList(true);
    if (globals_sp)
      variables.AddVariables(globals_sp.get());
  }

  for (size_t vi = 0, ve = variables.GetSize(); vi != ve; ++vi)
    if (auto error = AddVariableInfo(
            {variables.GetVariableAtIndex(vi)}, stack_frame_sp, ast_context,
            language_runtime, processed_names, local_variables, use_dynamic))
      return error;
  return {};
}

bool SwiftUserExpression::Parse(DiagnosticManager &diagnostic_manager,
                                ExecutionContext &exe_ctx,
                                lldb_private::ExecutionPolicy execution_policy,
                                bool keep_result_in_memory,
                                bool generate_debug_info) {
  Log *log = GetLog(LLDBLog::Expressions);
  LLDB_SCOPED_TIMER();

  Status err;

  InstallContext(exe_ctx);
  Target *target = exe_ctx.GetTargetPtr();
  if (!target) {
    diagnostic_manager.PutString(eDiagnosticSeverityError,
                                 "couldn't start parsing (no target)");
    return false;
  }

  StackFrame *frame = exe_ctx.GetFramePtr();
  if (!frame) {
    diagnostic_manager.PutString(eDiagnosticSeverityError,
                                 "couldn't start parsing - no stack frame");
    LLDB_LOG(log, "no stack frame");
    return false;
  }

  auto exe_scope = exe_ctx.GetBestExecutionContextScope();
  if (!exe_scope) {
    LLDB_LOG(log, "no execution context scope");
    return false;
  }

  m_swift_scratch_ctx = target->GetSwiftScratchContext(m_err, *exe_scope);
  if (!m_swift_scratch_ctx) {
    LLDB_LOG(log, "no scratch context", m_err.AsCString());
    return false;
  }
  m_swift_ast_ctx = llvm::dyn_cast_or_null<SwiftASTContextForExpressions>(
      m_swift_scratch_ctx->get()->GetSwiftASTContext());

  if (!m_swift_ast_ctx) {
    LLDB_LOG(log, "no Swift AST context");
    return false;
  }

  if (m_swift_ast_ctx->HasFatalErrors()) {
    LLDB_LOG(log, "Swift AST context is in a fatal error state");
    return false;
  }
  
  // This may destroy the scratch context.
  auto *persistent_state = GetPersistentState(target, exe_ctx);
  if (!persistent_state) {
    diagnostic_manager.PutString(eDiagnosticSeverityError,
                                 "couldn't start parsing (no persistent data)");
    return false;
  }

  Status error;
  SourceModule module_info;
  module_info.path.emplace_back("Swift");
  swift::ModuleDecl *module_decl =
      m_swift_ast_ctx->GetModule(module_info, error);

  if (error.Fail() || !module_decl) {
    LLDB_LOG(log, "couldn't load Swift Standard Library\n");
    return false;
  }

  persistent_state->AddHandLoadedModule(ConstString("Swift"),
                                        swift::ImportedModule(module_decl));
  m_result_delegate.RegisterPersistentState(persistent_state);
  m_error_delegate.RegisterPersistentState(persistent_state);
 
  ScanContext(exe_ctx, err);

  if (!err.Success()) {
    diagnostic_manager.Printf(eDiagnosticSeverityError, "warning: %s\n",
                              err.AsCString());
  }

  StreamString m_transformed_stream;

  //
  // Generate the expression.
  //

  std::string prefix = m_expr_prefix;

  std::unique_ptr<SwiftExpressionSourceCode> source_code(
      SwiftExpressionSourceCode::CreateWrapped(prefix.c_str(), 
                                               m_expr_text.c_str()));

  const lldb::LanguageType lang_type = lldb::eLanguageTypeSwift;

  m_options.SetLanguage(lang_type);
  m_options.SetGenerateDebugInfo(generate_debug_info);
  
  uint32_t first_body_line = 0;

  SymbolContext sc;
  lldb::StackFrameSP stack_frame;
  if (exe_scope) {
    lldb::TargetSP target_sp = exe_scope->CalculateTarget();

    stack_frame = exe_scope->CalculateStackFrame();

    if (stack_frame) {
      sc = stack_frame->GetSymbolContext(lldb::eSymbolContextEverything);
    } else {
      sc.target_sp = target_sp;
    }
  }
  llvm::SmallVector<SwiftASTManipulator::VariableInfo, 5> variables;
  RegisterAllVariables(sc, stack_frame, *m_swift_ast_ctx, variables,
                       m_options.GetUseDynamic());

  // I have to pass some value for add_locals.  I'm passing "false" because
  // even though we do add the locals, we don't do it in GetText.
  if (!source_code->GetText(m_transformed_text, lang_type,
                            m_needs_object_ptr,
                            m_in_static_method,
                            m_is_class,
                            m_is_weak_self,  m_options, exe_ctx,
                            first_body_line, variables)) {
    diagnostic_manager.PutString(eDiagnosticSeverityError,
                                  "couldn't construct expression body");
    return false;
  }

  if (log)
    log->Printf("Parsing the following code:\n%s", m_transformed_text.c_str());

  //
  // Parse the expression.
  //

  if (m_options.GetREPLEnabled())
    m_materializer_up.reset(new SwiftREPLMaterializer());
  else
    m_materializer_up.reset(new Materializer());

  auto *swift_parser =
      new SwiftExpressionParser(exe_scope, *m_swift_ast_ctx, *this, m_options);
  unsigned error_code = swift_parser->Parse(
      diagnostic_manager, variables, first_body_line,
      first_body_line + source_code->GetNumBodyLines());
  m_parser.reset(swift_parser);

  if (error_code == 2) {
    m_fixed_text = m_expr_text;
    return false;
  } else if (error_code) {
    // Calculate the fixed expression string at this point:
    if (diagnostic_manager.HasFixIts()) {
      if (m_parser->RewriteExpression(diagnostic_manager)) {
        size_t fixed_start;
        size_t fixed_end;
        const std::string &fixed_expression =
            diagnostic_manager.GetFixedExpression();
        if (SwiftExpressionSourceCode::GetOriginalBodyBounds(
                fixed_expression, fixed_start, fixed_end))
          m_fixed_text =
              fixed_expression.substr(fixed_start, fixed_end - fixed_start);
      }
    }
    return false;
  }


  // Prepare the output of the parser for execution, evaluating it
  // statically if possible.
  Status jit_error = m_parser->PrepareForExecution(
      m_jit_start_addr, m_jit_end_addr, m_execution_unit_sp, exe_ctx,
      m_can_interpret, execution_policy);

  if (m_execution_unit_sp) {
    if (m_options.GetREPLEnabled()) {
      llvm::cast<SwiftREPLMaterializer>(m_materializer_up.get())
          ->RegisterExecutionUnit(m_execution_unit_sp.get());
    }

    bool register_execution_unit = false;

    if (m_options.GetREPLEnabled()) {
      if (!m_execution_unit_sp->GetJittedFunctions().empty() ||
          !m_execution_unit_sp->GetJittedGlobalVariables().empty()) {
        register_execution_unit = true;
      }
    } else {
      if (m_execution_unit_sp->GetJittedFunctions().size() > 1 ||
          m_execution_unit_sp->GetJittedGlobalVariables().size() > 1) {
        register_execution_unit = true;
      }
    }

    if (register_execution_unit) {
      // We currently key off there being more than one external
      // function in the execution unit to determine whether it needs
      // to live in the process.
      GetPersistentState(exe_ctx.GetTargetPtr(), exe_ctx)
          ->RegisterExecutionUnit(m_execution_unit_sp);
    }
  }

  StreamString jit_module_name;
  jit_module_name.Printf("%s%u", FunctionName(),
                         m_options.GetExpressionNumber());
  auto module =
      m_execution_unit_sp->CreateJITModule(jit_module_name.GetString().data());

  Process *process = exe_ctx.GetProcessPtr();
  auto *swift_runtime = SwiftLanguageRuntime::Get(process);
  if (module && swift_runtime) {
    ModuleList modules;
    modules.Append(module, false);
    swift_runtime->ModulesDidLoad(modules);
  }

  if (jit_error.Success()) {
    if (process && m_jit_start_addr != LLDB_INVALID_ADDRESS)
      m_jit_process_wp = lldb::ProcessWP(process->shared_from_this());
    return true;
  } else {
    const char *error_cstr = jit_error.AsCString();
    if (error_cstr && error_cstr[0])
      diagnostic_manager.PutString(eDiagnosticSeverityError, error_cstr);
    else
      diagnostic_manager.PutString(eDiagnosticSeverityError,
                                    "expression can't be interpreted or run\n");
    return false;
  }
}

bool SwiftUserExpression::AddArguments(ExecutionContext &exe_ctx,
                                       std::vector<lldb::addr_t> &args,
                                       lldb::addr_t struct_address,
                                       DiagnosticManager &diagnostic_manager) {
  if (m_options.GetPlaygroundTransformEnabled() || m_options.GetREPLEnabled()) {
    // When calling the playground function we are calling a main
    // function which takes two arguments: argc and argv So we pass
    // two zeroes as arguments.
    args.push_back(0); // argc
    args.push_back(0); // argv
    return true;
  }
  args.push_back(struct_address);
  return true;
}

lldb::ExpressionVariableSP SwiftUserExpression::GetResultAfterDematerialization(
    ExecutionContextScope *exe_scope) {
  LLDB_SCOPED_TIMER();
  lldb::ExpressionVariableSP in_result_sp = m_result_delegate.GetVariable();
  lldb::ExpressionVariableSP in_error_sp = m_error_delegate.GetVariable();

  lldb::ExpressionVariableSP result_sp;

  if (in_error_sp) {
    bool error_is_valid = false;

    if (in_error_sp->GetCompilerType().GetTypeSystem()->SupportsLanguage(
            lldb::eLanguageTypeSwift)) {
      lldb::ValueObjectSP val_sp = in_error_sp->GetValueObject();
      if (val_sp) {
        if (exe_scope) {
          lldb::ProcessSP process_sp = exe_scope->CalculateProcess();
          if (process_sp) {
            auto *swift_runtime = SwiftLanguageRuntime::Get(process_sp);
            if (swift_runtime)
              error_is_valid = swift_runtime->IsValidErrorValue(*val_sp.get());
          }
        }
      }
    }

    lldb::TargetSP target_sp = exe_scope->CalculateTarget();

    if (target_sp) {
      if (auto *persistent_state =
              target_sp->GetSwiftPersistentExpressionState(*exe_scope)) {
        if (error_is_valid) {
          persistent_state->RemovePersistentVariable(in_result_sp);
          result_sp = in_error_sp;
        } else {
          persistent_state->RemovePersistentVariable(in_error_sp);
          result_sp = in_result_sp;
        }
      }
    }
  } else
    result_sp = in_result_sp;

  return result_sp;
}

SwiftUserExpression::ResultDelegate::ResultDelegate(
    lldb::TargetSP target, SwiftUserExpression &, bool is_error)
    : m_target_sp(target), m_is_error(is_error) {}

ConstString SwiftUserExpression::ResultDelegate::GetName() {
  return m_persistent_state->GetNextPersistentVariableName(m_is_error);
}

void SwiftUserExpression::ResultDelegate::DidDematerialize(
    lldb::ExpressionVariableSP &variable) {
  m_variable = variable;
}

void SwiftUserExpression::ResultDelegate::RegisterPersistentState(
    PersistentExpressionState *persistent_state) {
  m_persistent_state = persistent_state;
}

lldb::ExpressionVariableSP &SwiftUserExpression::ResultDelegate::GetVariable() {
  return m_variable;
}

SwiftUserExpression::PersistentVariableDelegate::PersistentVariableDelegate(
    SwiftUserExpression &) {}

ConstString SwiftUserExpression::PersistentVariableDelegate::GetName() {
  return ConstString();
}

void SwiftUserExpression::PersistentVariableDelegate::DidDematerialize(
    lldb::ExpressionVariableSP &variable) {
  if (SwiftExpressionVariable *swift_var =
          llvm::dyn_cast<SwiftExpressionVariable>(variable.get())) {
    swift_var->m_swift_flags &= ~SwiftExpressionVariable::EVSNeedsInit;
  }
}
