//===-- BreakpointLocation.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Breakpoint/BreakpointLocation.h"
#include "lldb/Breakpoint/BreakpointID.h"
#include "lldb/Breakpoint/BreakpointResolver.h"
#include "lldb/Breakpoint/BreakpointResolverScripted.h"
#include "lldb/Breakpoint/StoppointCallbackContext.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/Module.h"
#include "lldb/Expression/DiagnosticManager.h"
#include "lldb/Expression/ExpressionVariable.h"
#include "lldb/Expression/UserExpression.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/Symbol.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/TypeSystem.h"
#include "lldb/Target/FunctionPatch.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"
#include "lldb/Target/ThreadSpec.h"
#include "lldb/Utility/Baton.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Policy.h"
#include "lldb/Utility/StreamString.h"
#include "lldb/ValueObject/ValueObject.h"

using namespace lldb;
using namespace lldb_private;

namespace {
/// What the site of a compiled-in condition's trap needs in order to find the
/// location whose condition it is.
///
/// Weakly, and by id rather than by pointer, because a patch outlives the
/// breakpoint that asked for it: the trap stays in the program's code after the
/// user deletes the breakpoint.
struct InProcessConditionOwner {
  lldb::BreakpointWP breakpoint_wp;
  lldb::break_id_t loc_id = LLDB_INVALID_BREAK_ID;

  /// The injection whose trap this is, so that a trap which finds its breakpoint
  /// gone can take that injection back out. Set once the install has succeeded,
  /// since that is when there is a site to name.
  std::optional<uint32_t> site_id;
};

/// Whether \p addr is in code the debugger compiled rather than in the program.
///
/// A patched copy is compiled from the program's own source and its line table
/// points back at that source, so it answers a file-and-line lookup exactly as
/// the program does. Where the code came from is what tells them apart.
bool IsInDebuggerCompiledCode(const Address &addr) {
  ModuleSP module_sp = addr.GetModule();
  if (!module_sp)
    return false;
  ObjectFile *object_file = module_sp->GetObjectFile();
  return object_file && object_file->GetType() == ObjectFile::eTypeJIT;
}
} // namespace

BreakpointLocation::BreakpointLocation(break_id_t loc_id, Breakpoint &owner,
                                       const Address &addr, lldb::tid_t tid,
                                       bool check_for_resolver)
    : m_address(addr), m_owner(owner), m_loc_id(loc_id) {
  if (check_for_resolver) {
    const Symbol *symbol = m_address.CalculateSymbolContextSymbol();
    if (symbol && symbol->IsIndirect()) {
      SetShouldResolveIndirectFunctions(true);
    }
  }

  SetThreadIDInternal(tid);
}

BreakpointLocation::BreakpointLocation(break_id_t loc_id, Breakpoint &owner)
    : m_owner(owner), m_loc_id(loc_id) {
  SetThreadIDInternal(LLDB_INVALID_THREAD_ID);
}

BreakpointLocation::~BreakpointLocation() {
  llvm::consumeError(ClearBreakpointSite());
}

lldb::addr_t BreakpointLocation::GetLoadAddress() const {
  return m_address.GetOpcodeLoadAddress(&m_owner.GetTarget());
}

const BreakpointOptions &BreakpointLocation::GetOptionsSpecifyingKind(
    BreakpointOptions::OptionKind kind) const {
  if (m_options_up && m_options_up->IsOptionSet(kind))
    return *m_options_up;
  return m_owner.GetOptions();
}

Address &BreakpointLocation::GetAddress() { return m_address; }

Breakpoint &BreakpointLocation::GetBreakpoint() { return m_owner; }

Target &BreakpointLocation::GetTarget() { return m_owner.GetTarget(); }

bool BreakpointLocation::IsEnabled() const {
  if (!m_owner.IsEnabled())
    return false;
  if (m_options_up != nullptr)
    return m_options_up->IsEnabled();
  return true;
}

llvm::Error BreakpointLocation::SetEnabled(bool enabled) {
  GetLocationOptions().SetEnabled(enabled);
  llvm::Error error = enabled ? ResolveBreakpointSite() : ClearBreakpointSite();
  SendBreakpointLocationChangedEvent(enabled ? eBreakpointEventTypeEnabled
                                             : eBreakpointEventTypeDisabled);
  return error;
}

bool BreakpointLocation::IsAutoContinue() const {
  if (m_options_up &&
      m_options_up->IsOptionSet(BreakpointOptions::eAutoContinue))
    return m_options_up->IsAutoContinue();
  return m_owner.IsAutoContinue();
}

void BreakpointLocation::SetAutoContinue(bool auto_continue) {
  GetLocationOptions().SetAutoContinue(auto_continue);
  SendBreakpointLocationChangedEvent(eBreakpointEventTypeAutoContinueChanged);
}

void BreakpointLocation::SetThreadID(lldb::tid_t thread_id) {
  SetThreadIDInternal(thread_id);
  SendBreakpointLocationChangedEvent(eBreakpointEventTypeThreadChanged);
}

lldb::tid_t BreakpointLocation::GetThreadID() {
  const ThreadSpec *thread_spec =
      GetOptionsSpecifyingKind(BreakpointOptions::eThreadSpec)
          .GetThreadSpecNoCreate();
  if (thread_spec)
    return thread_spec->GetTID();
  return LLDB_INVALID_THREAD_ID;
}

void BreakpointLocation::SetThreadIndex(uint32_t index) {
  if (index != 0)
    GetLocationOptions().GetThreadSpec()->SetIndex(index);
  else {
    // If we're resetting this to an invalid thread id, then don't make an
    // options pointer just to do that.
    if (m_options_up != nullptr)
      m_options_up->GetThreadSpec()->SetIndex(index);
  }
  SendBreakpointLocationChangedEvent(eBreakpointEventTypeThreadChanged);
}

uint32_t BreakpointLocation::GetThreadIndex() const {
  const ThreadSpec *thread_spec =
      GetOptionsSpecifyingKind(BreakpointOptions::eThreadSpec)
          .GetThreadSpecNoCreate();
  if (thread_spec)
    return thread_spec->GetIndex();
  return 0;
}

void BreakpointLocation::SetThreadName(const char *thread_name) {
  if (thread_name != nullptr)
    GetLocationOptions().GetThreadSpec()->SetName(thread_name);
  else {
    // If we're resetting this to an invalid thread id, then don't make an
    // options pointer just to do that.
    if (m_options_up != nullptr)
      m_options_up->GetThreadSpec()->SetName(thread_name);
  }
  SendBreakpointLocationChangedEvent(eBreakpointEventTypeThreadChanged);
}

const char *BreakpointLocation::GetThreadName() const {
  const ThreadSpec *thread_spec =
      GetOptionsSpecifyingKind(BreakpointOptions::eThreadSpec)
          .GetThreadSpecNoCreate();
  if (thread_spec)
    return thread_spec->GetName();
  return nullptr;
}

void BreakpointLocation::SetQueueName(const char *queue_name) {
  if (queue_name != nullptr)
    GetLocationOptions().GetThreadSpec()->SetQueueName(queue_name);
  else {
    // If we're resetting this to an invalid thread id, then don't make an
    // options pointer just to do that.
    if (m_options_up != nullptr)
      m_options_up->GetThreadSpec()->SetQueueName(queue_name);
  }
  SendBreakpointLocationChangedEvent(eBreakpointEventTypeThreadChanged);
}

const char *BreakpointLocation::GetQueueName() const {
  const ThreadSpec *thread_spec =
      GetOptionsSpecifyingKind(BreakpointOptions::eThreadSpec)
          .GetThreadSpecNoCreate();
  if (thread_spec)
    return thread_spec->GetQueueName();
  return nullptr;
}

bool BreakpointLocation::InvokeCallback(StoppointCallbackContext *context) {
  if (m_options_up != nullptr && m_options_up->HasCallback())
    return m_options_up->InvokeCallback(context, m_owner.GetID(), GetID());
  return m_owner.InvokeCallback(context, GetID());
}

bool BreakpointLocation::IsCallbackSynchronous() {
  if (m_options_up != nullptr && m_options_up->HasCallback())
    return m_options_up->IsCallbackSynchronous();
  return m_owner.GetOptions().IsCallbackSynchronous();
}

void BreakpointLocation::SetCallback(BreakpointHitCallback callback,
                                     void *baton, bool is_synchronous) {
  // The default "Baton" class will keep a copy of "baton" and won't free or
  // delete it when it goes out of scope.
  GetLocationOptions().SetCallback(
      callback, std::make_shared<UntypedBaton>(baton), is_synchronous);
  SendBreakpointLocationChangedEvent(eBreakpointEventTypeCommandChanged);
}

void BreakpointLocation::SetCallback(BreakpointHitCallback callback,
                                     const BatonSP &baton_sp,
                                     bool is_synchronous) {
  GetLocationOptions().SetCallback(callback, baton_sp, is_synchronous);
  SendBreakpointLocationChangedEvent(eBreakpointEventTypeCommandChanged);
}

void BreakpointLocation::ClearCallback() {
  GetLocationOptions().ClearCallback();
}

void BreakpointLocation::SetCondition(StopCondition condition) {
  GetLocationOptions().SetCondition(std::move(condition));
  SendBreakpointLocationChangedEvent(eBreakpointEventTypeConditionChanged);
  CompileConditionIntoProcess();
}

const StopCondition &BreakpointLocation::GetCondition() const {
  return GetOptionsSpecifyingKind(BreakpointOptions::eCondition).GetCondition();
}

void BreakpointLocation::CompileConditionIntoProcess() {
  // The text the copy has to be testing for this location to behave as it is set
  // up to behave. Empty when there is no condition, which after one has been
  // compiled in is a change like any other: the copy has to stop testing what it
  // was given.
  const llvm::StringRef wanted = GetCondition().GetText();

  // Nothing asked for and nothing compiled in, which is every location of every
  // breakpoint that has no condition: two empty checks per stop and no more.
  if (wanted.empty() && !m_in_process_site_id)
    return;

  // Asked once per text, whichever way the asking went. A refusal is a property
  // of this location and this program rather than of the moment, and retrying at
  // every stop would pay to recompile the function over and over only to be
  // refused each time.
  if (m_in_process_condition_attempted &&
      *m_in_process_condition_attempted == wanted)
    return;

  // The setting says whether a condition may be compiled in at all. It does not
  // say anything about a copy that is already testing one: turning it off does
  // not put a redirected entry back, so a location that has an injection has to
  // be kept in step with the condition it holds either way. Otherwise an edit
  // made after the setting was turned off would leave the copy testing the text
  // it replaced, and nothing would say so.
  if (!m_in_process_site_id) {
    ExecutionContext exe_ctx(GetTarget().shared_from_this(), false);
    if (!GetTarget().GetFastConditions(&exe_ctx))
      return;
  }

  // A location in code the debugger compiled is a location a patch produced:
  // the copy's line table points back at the original source, so the breakpoint
  // resolves into the copy as well. There is nothing to compile in for it, and
  // recompiling from a copy would patch a patch, so this is not a refusal to
  // report.
  if (IsInDebuggerCompiledCode(GetAddress())) {
    m_in_process_condition_attempted = wanted.str();
    return;
  }

  // Not yet rather than no: a condition is set long before the program is at a
  // stop that a patch would survive, so this is asked again at every stop until
  // one is. An edit waits the same way, and until it is served the copy goes on
  // testing the text it was built with -- so a hit the replaced text selected can
  // still arrive at the stop that serves the edit.
  if (!GetTarget().CanCompileCodeIntoProcess())
    return;

  m_in_process_condition_attempted = wanted.str();
  // Its own copy from here on, since what follows recompiles the function and
  // resolves breakpoints against the result, and a reference into this location's
  // own state is not worth having to reason about across all of that.
  const std::string wanted_text = wanted.str();

  // A copy tests the text it was compiled with, so the injection carrying the
  // old text comes out before one carrying the new text goes in. Nothing here can
  // amend a copy in place: each half is a recompile of the function.
  if (m_in_process_site_id) {
    if (llvm::Error removed =
            GetTarget().GetFunctionPatchManager().Remove(*m_in_process_site_id)) {
      // Reported rather than worked around, and the site kept: a removal that
      // failed left the injection carrying the replaced text in the program,
      // still able to stop, which is a location that tests an out-of-date
      // condition. Forgetting the site would make it a location that cannot stop
      // at all, since its own trap is in a body the redirect no longer reaches.
      m_condition_not_compiled_reason = llvm::toString(std::move(removed));
      LLDB_LOG(GetLog(LLDBLog::Breakpoints),
               "the compiled-in condition this location replaced could not be "
               "taken back out of the program, so the program is still testing "
               "it: {0}",
               m_condition_not_compiled_reason);
      return;
    }
    m_in_process_site_id.reset();
  }

  llvm::Error error = InstallInProcessCondition(wanted_text);
  if (!error) {
    // Cleared, so that a location asked again with text it could compile in does
    // not go on saying that it could not.
    m_condition_not_compiled_reason.clear();
    return;
  }

  // Kept rather than only logged. Every way this can fail leaves the condition
  // to be evaluated at a stop, so a refusal costs speed rather than correctness
  // -- but the speed is the whole point, so whoever asked has to be able to
  // read back why they did not get it.
  m_condition_not_compiled_reason = llvm::toString(std::move(error));
  LLDB_LOG(GetLog(LLDBLog::Breakpoints),
           "condition not compiled into the process: {0}",
           m_condition_not_compiled_reason);
}

void BreakpointLocation::ForgetConditionCompiledIntoProcess() {
  m_in_process_site_id.reset();
  m_in_process_condition_attempted.reset();
  m_condition_not_compiled_reason.clear();
}

llvm::Error
BreakpointLocation::InstallInProcessCondition(llvm::StringRef condition_text) {
  if (IsFacade())
    return llvm::createStringError(
        "a facade location stands for no code that could be recompiled");

  // A scripted breakpoint answers a hit by naming which of its locations the hit
  // belongs to, and only its resolver can decide that. A trap in the copy has
  // nowhere to ask, so the hit would be credited to the location that was
  // patched rather than to the one the resolver would have chosen.
  if (BreakpointResolverSP resolver_sp = GetBreakpoint().GetResolver())
    if (resolver_sp->getResolverID() == BreakpointResolver::PythonResolver)
      return llvm::createStringError(
          "a scripted breakpoint decides for itself which of its locations a "
          "hit belongs to, which a trap in a recompiled copy cannot ask it");

  SymbolContext sc;
  GetAddress().CalculateSymbolContext(&sc, eSymbolContextFunction |
                                               eSymbolContextBlock |
                                               eSymbolContextLineEntry);
  if (!sc.function || sc.line_entry.line == 0)
    return llvm::createStringError(
        "no debug info describes a function and line at the location");

  // An injection is placed by line into text read from the function's own source
  // file, and the function this location is in is the one whose entry a redirect
  // would be written over. A location inside an inlined instance answers those
  // two questions from different functions: the line is the inlined callee's,
  // where the condition's names are in scope, and the function is the caller the
  // callee was inlined into. Recompiling the caller with the callee's line puts
  // the injection at whatever the caller has at that line number -- an unrelated
  // statement, and in a different file whenever the callee came from a header.
  //
  // Which is why the line entry's own file is checked too, rather than only the
  // block: the file is the thing the injection is spliced into, so a line entry
  // naming another file is the same mistake by another route.
  if (sc.block && sc.block->GetContainingInlinedBlock())
    return llvm::createStringError(
        "the location is inside an inlined instance, whose line belongs to the "
        "function that was inlined rather than to the one a redirect would be "
        "written over");

  SupportFileNSP declared_in = std::make_shared<SupportFile>();
  uint32_t declared_at = 0;
  sc.function->GetStartLineSourceInfo(declared_in, declared_at);
  if (!sc.line_entry.file_sp ||
      !sc.line_entry.file_sp->Equal(*declared_in,
                                    SupportFile::eEqualFileSpecAndChecksumIfSet))
    return llvm::createStringError(
        llvm::Twine("the line at the location is in \"") +
        (sc.line_entry.file_sp
             ? sc.line_entry.file_sp->GetSpecOnly().GetPath()
             : "no file") +
        "\", where the function it is in is declared in \"" +
        declared_in->GetSpecOnly().GetPath() +
        "\", so a line of the one is not a line of the other");

  assert(!IsInDebuggerCompiledCode(GetAddress()) &&
         "recompiling a copy the debugger compiled would patch a patch");

  PatchRequest request;
  request.FunctionEntry =
      sc.function->GetAddress().GetLoadAddress(&GetTarget());
  request.Line = sc.line_entry.line;
  // No condition at all rather than an empty one, which is what a location whose
  // condition has been cleared needs: it must stop on every hit, and its own trap
  // sits in a body the redirect no longer reaches.
  if (!condition_text.empty())
    request.Condition = condition_text.str();
  request.WantStop = true;
  request.OnTrap = ForwardInProcessTrap;
  // Named, so that this breakpoint's own locations in the body about to stop
  // being reached are not counted as breakpoints the patch would orphan. Their
  // hits are what the injection exists to deliver.
  request.HitsCarriedBy = m_owner.GetID();

  auto owner = std::make_unique<InProcessConditionOwner>();
  owner->breakpoint_wp = GetBreakpoint().shared_from_this();
  owner->loc_id = GetID();
  auto baton =
      std::make_shared<TypedBaton<InProcessConditionOwner>>(std::move(owner));
  // Held past the move, so that the site id can be written into the baton once
  // there is one: the trap that carries it is what discovers the breakpoint has
  // been deleted, and taking the injection out then needs its name.
  InProcessConditionOwner &trap_owner = *baton->getItem();
  request.Baton = std::move(baton);

  // Marked before the patch is installed, not after: announcing the copy is
  // part of installing it, and that is the moment this breakpoint acquires a
  // location inside the copy that must not be armed.
  const bool was_compiled_in = m_owner.m_condition_compiled_into_process;
  m_owner.m_condition_compiled_into_process = true;

  llvm::Expected<uint32_t> site_id =
      GetTarget().GetFunctionPatchManager().Install(request);
  if (!site_id) {
    m_owner.m_condition_compiled_into_process = was_compiled_in;
    // A refusal ordinarily costs speed and nothing else: the condition goes
    // back to being evaluated at this location's own stop. Not when the
    // function is already redirected to a copy compiled without this condition,
    // since the original body this location sits in is no longer reached.
    // Reporting only why the compile failed would understate that.
    if (GetTarget().GetFunctionPatchManager().IsPatched(
            request.FunctionEntry)) {
      // Trimmed because a compiler diagnostic arrives with its own trailing
      // newline, and what follows is the same sentence.
      std::string why = llvm::toString(site_id.takeError());
      return llvm::createStringError(
          llvm::StringRef(why).rtrim() +
          "; the function is already redirected to a copy compiled without it, "
          "so this location is no longer reached");
    }
    return site_id.takeError();
  }

  // The original body is unreachable once the entry is redirected, so this
  // location's own trap can never fire again. It is left in place all the same:
  // a disabled location reports no hits, and the hits the compiled-in condition
  // trapped for are forwarded to it.
  m_in_process_site_id = *site_id;
  trap_owner.site_id = *site_id;
  return llvm::Error::success();
}

bool BreakpointLocation::ForwardInProcessTrap(void *baton,
                                              StoppointCallbackContext *context,
                                              lldb::user_id_t break_id,
                                              lldb::user_id_t break_loc_id) {
  auto *owner = static_cast<InProcessConditionOwner *>(baton);
  BreakpointSP bp_sp = owner->breakpoint_wp.lock();
  BreakpointLocationSP loc_sp =
      bp_sp ? bp_sp->FindLocationByID(owner->loc_id) : nullptr;
  TargetSP target_sp = context->exe_ctx_ref.GetTargetSP();

  // Still the target's, rather than merely still alive. A deleted breakpoint can
  // be held alive by anything that took a reference to it -- an SB object
  // outliving the delete is the ordinary case -- and its hits are nobody's from
  // the moment the target stops knowing about it.
  const bool still_set = bp_sp && target_sp &&
                         target_sp->GetBreakpointByID(bp_sp->GetID()) == bp_sp;

  if (!loc_sp || !still_set) {
    // Taken out of the program rather than left trapping for nobody. Every hit
    // from here on would otherwise cost a stop that ends exactly here, which is
    // the cost that compiling the condition in was for. Discovered at a trap
    // rather than when the breakpoint went away: this is the first moment the
    // program is held still with the injection known to belong to no one.
    if (owner->site_id && target_sp) {
      llvm::Error error =
          target_sp->GetFunctionPatchManager().Remove(*owner->site_id);
      owner->site_id.reset();
      if (error)
        LLDB_LOG_ERROR(GetLog(LLDBLog::Breakpoints), std::move(error),
                       "the compiled-in condition of a breakpoint that is gone "
                       "could not be taken out of the program: {0}");
    }
    return false;
  }

  return loc_sp->ReportForwardedHit(context);
}

bool BreakpointLocation::ReportForwardedHit(StoppointCallbackContext *context) {
  // Everything an ordinary hit is put through, bar the condition: it has already
  // held -- that is why the trap executed -- where an ordinary hit still has its
  // condition ahead of it. Compiling a condition in moves where the condition is
  // evaluated and changes nothing else, so a hit arriving this way has to meet
  // every other test the breakpoint carries.
  if (!IsEnabled())
    return false;

  lldb::ThreadSP thread_sp = context->exe_ctx_ref.GetThreadSP();
  if (thread_sp && !ValidForThisThread(*thread_sp))
    return false;

  BumpHitCount();

  // A hit that arrives while an expression is running is still this location's
  // hit, but the actions that follow one must not run there: a callback or a
  // condition that called the same function again would recurse. The ordinary
  // path answers this by asking the process whether breakpoints are ignored
  // inside expressions, and so does this one -- otherwise a call to a patched
  // function is interrupted where the same call, with the condition evaluated at
  // a stop, returns a value.
  if (!PolicyStack::Get().Current().capabilities.can_run_breakpoint_actions) {
    Process *process = GetTarget().GetProcessSP().get();
    return process && !process->GetIgnoreBreakpointsInExpressions();
  }

  if (!IgnoreCountShouldStop())
    return false;

  bool should_stop = true;
  if (IsAutoContinue()) {
    // Reported even though it does not stop, which is how somebody reading the
    // run learns that it auto-continued.
    if (thread_sp && !GetBreakpoint().IsInternal())
      thread_sp->SetShouldReportStop(eVoteYes);
    should_stop = false;
  }

  // Offered in the phase the callback asked for rather than in the one this trap
  // arrived in. A callback is offered a hit once, and a synchronous one offered
  // it under an asynchronous context declines it -- which would swallow the stop
  // rather than delay it.
  StoppointCallbackContext forwarded(*context);
  forwarded.is_synchronous = IsCallbackSynchronous();
  if (!InvokeCallback(&forwarded))
    return false;

  // Removed here rather than at the stop, because the stop this hit produces is
  // attributed to the internal breakpoint carrying the trap; nothing downstream
  // knows a one-shot breakpoint was the reason for it.
  if (GetBreakpoint().IsOneShot())
    GetTarget().RemoveBreakpointByID(GetBreakpoint().GetID());

  // And named as an ordinary hit is named, for the same reason. The stop belongs
  // to the internal breakpoint that carries the trap, and an internal
  // breakpoint's stop reason is its kind -- so a program stopped here reported
  // stopping for "in-process-condition", which is a fact about the debugger's
  // machinery rather than the breakpoint the user set and can act on.
  if (should_stop && thread_sp) {
    if (lldb::StopInfoSP stop_info_sp = thread_sp->GetStopInfo()) {
      StreamString strm;
      strm.PutCString("breakpoint ");
      BreakpointID::GetCanonicalReference(&strm, GetBreakpoint().GetID(),
                                         GetID());
      stop_info_sp->SetDescription(strm.GetData());
    }
  }

  return should_stop;
}

bool BreakpointLocation::ConditionSaysStop(ExecutionContext &exe_ctx,
                                           Status &error) {
  Log *log = GetLog(LLDBLog::Breakpoints);

  std::lock_guard<std::mutex> guard(m_condition_mutex);

  StopCondition condition = GetCondition();

  if (!condition) {
    m_user_expression_sp.reset();
    return false;
  }

  error.Clear();

  DiagnosticManager diagnostics;

  if (condition.GetHash() != m_condition_hash || !m_user_expression_sp ||
      !m_user_expression_sp->IsParseCacheable() ||
      !m_user_expression_sp->MatchesContext(exe_ctx)) {
    LanguageType language = condition.GetLanguage();
    if (language == lldb::eLanguageTypeUnknown) {
      // See if we can figure out the language from the frame, otherwise use the
      // default language:
      if (CompileUnit *comp_unit =
              m_address.CalculateSymbolContextCompileUnit())
        language = comp_unit->GetLanguage();
    }

    m_user_expression_sp.reset(GetTarget().GetUserExpressionForLanguage(
        condition.GetText(), llvm::StringRef(), SourceLanguage{language},
        Expression::eResultTypeAny, EvaluateExpressionOptions(), nullptr,
        error));
    if (error.Fail()) {
      LLDB_LOGF(log, "Error getting condition expression: %s.",
                error.AsCString());
      m_user_expression_sp.reset();
      return true;
    }

    if (!m_user_expression_sp->Parse(diagnostics, exe_ctx,
                                     eExecutionPolicyOnlyWhenNeeded, true,
                                     false)) {
      error = Status::FromError(
          diagnostics.GetAsError(lldb::eExpressionParseError,
                                 "Couldn't parse conditional expression:"));

      m_user_expression_sp.reset();
      return true;
    }

    m_condition_hash = condition.GetHash();
  }

  // We need to make sure the user sees any parse errors in their condition, so
  // we'll hook the constructor errors up to the debugger's Async I/O.

  ValueObjectSP result_value_sp;

  EvaluateExpressionOptions options;
  options.SetUnwindOnError(true);
  options.SetIgnoreBreakpoints(true);
  options.SetTryAllThreads(true);
  options.SetSuppressPersistentResult(
      true); // Don't generate a user variable for condition expressions.

  Status expr_error;

  diagnostics.Clear();

  ExpressionVariableSP result_variable_sp;

  ExpressionResults result_code = m_user_expression_sp->Execute(
      diagnostics, exe_ctx, options, m_user_expression_sp, result_variable_sp);

  bool ret;

  if (result_code == eExpressionCompleted) {
    if (!result_variable_sp) {
      error = Status::FromErrorString("Expression did not return a result");
      return false;
    }

    result_value_sp = result_variable_sp->GetValueObject();

    if (result_value_sp) {
      ret = result_value_sp->IsLogicalTrue(error);
      if (log) {
        if (error.Success()) {
          LLDB_LOGF(log, "Condition successfully evaluated, result is %s.\n",
                    ret ? "true" : "false");
        } else {
          error = Status::FromErrorString(
              "Failed to get an integer result from the expression");
          ret = false;
        }
      }
    } else {
      ret = false;
      error = Status::FromErrorString(
          "Failed to get any result from the expression");
    }
  } else {
    ret = false;
    error = Status::FromError(diagnostics.GetAsError(
        lldb::eExpressionParseError, "Couldn't execute expression:"));
  }

  return ret;
}

uint32_t BreakpointLocation::GetIgnoreCount() const {
  return GetOptionsSpecifyingKind(BreakpointOptions::eIgnoreCount)
      .GetIgnoreCount();
}

void BreakpointLocation::SetIgnoreCount(uint32_t n) {
  GetLocationOptions().SetIgnoreCount(n);
  SendBreakpointLocationChangedEvent(eBreakpointEventTypeIgnoreChanged);
}

void BreakpointLocation::DecrementIgnoreCount() {
  if (m_options_up != nullptr) {
    uint32_t loc_ignore = m_options_up->GetIgnoreCount();
    if (loc_ignore != 0)
      m_options_up->SetIgnoreCount(loc_ignore - 1);
  }
}

bool BreakpointLocation::IgnoreCountShouldStop() {
  uint32_t owner_ignore = GetBreakpoint().GetIgnoreCount();
  uint32_t loc_ignore = 0;
  if (m_options_up != nullptr)
    loc_ignore = m_options_up->GetIgnoreCount();

  if (loc_ignore != 0 || owner_ignore != 0) {
    m_owner.DecrementIgnoreCount();
    DecrementIgnoreCount(); // Have to decrement our owners' ignore count,
                            // since it won't get a chance to.
    return false;
  }
  return true;
}

BreakpointOptions &BreakpointLocation::GetLocationOptions() {
  // If we make the copy we don't copy the callbacks because that is
  // potentially expensive and we don't want to do that for the simple case
  // where someone is just disabling the location.
  if (m_options_up == nullptr)
    m_options_up = std::make_unique<BreakpointOptions>(false);

  return *m_options_up;
}

bool BreakpointLocation::ValidForThisThread(Thread &thread) {
  return thread.MatchesSpec(
      GetOptionsSpecifyingKind(BreakpointOptions::eThreadSpec)
          .GetThreadSpecNoCreate());
}

BreakpointLocationSP
BreakpointLocation::WasHit(StoppointCallbackContext *context) {
  // Only the BreakpointResolverScripted provides WasHit.
  BreakpointResolverSP resolver_sp = GetBreakpoint().GetResolver();
  BreakpointResolverScripted *scripted =
      llvm::dyn_cast<BreakpointResolverScripted>(resolver_sp.get());
  if (!scripted)
    return shared_from_this();

  StackFrameSP frame_sp = context->exe_ctx_ref.GetFrameSP();
  if (!frame_sp)
    return shared_from_this();

  BreakpointLocationSP return_loc_sp =
      scripted->WasHit(frame_sp, shared_from_this());
  // If this is a facade location, then we won't have bumped its hit count
  // while processing the original location hit.  Do so here.  We don't need
  // to bump the breakpoint's hit count, however, since hitting the real
  // location would have already done that.
  // Also we have to check the enabled state here, since we would never have
  // gotten here with a real location...
  if (return_loc_sp && return_loc_sp->IsFacade()) {
    if (return_loc_sp->IsEnabled())
      return_loc_sp->m_hit_counter.Increment();
    else
      return {};
  }
  return return_loc_sp;
}

// RETURNS - true if we should stop at this breakpoint, false if we
// should continue.  Note, we don't check the thread spec for the breakpoint
// here, since if the breakpoint is not for this thread, then the event won't
// even get reported, so the check is redundant.

bool BreakpointLocation::ShouldStop(StoppointCallbackContext *context,
                                    lldb::BreakpointLocationSP &facade_loc_sp) {
  bool should_stop = true;
  Log *log = GetLog(LLDBLog::Breakpoints);

  // Do this first, if a location is disabled, it shouldn't increment its hit
  // count.
  if (!IsEnabled())
    return false;

  // Next check WasHit:
  BreakpointLocationSP loc_hit_sp = WasHit(context);

  if (!loc_hit_sp) {
    // We bump the hit counts in StopInfoBreakpoint::ShouldStopSynchronous,
    // before we call into each location's ShouldStop.  So we need to undo
    // that here.
    UndoBumpHitCount();
    return false;
  }

  // If the location hit was not us, it was a facade location, in which case
  // we should use the facade location's callbacks, etc.  Those will all be
  // run in the asynchronous phase, so for now we just have to record the fact
  // that we should treat this as a facade hit.  This is strictly an out
  // parameter, so clear it if this isn't a facade hit.
  if (loc_hit_sp.get() != this)
    facade_loc_sp = loc_hit_sp;
  else
    facade_loc_sp.reset();

  // We only run synchronous callbacks in ShouldStop:
  context->is_synchronous = true;
  should_stop = InvokeCallback(context);

  if (log) {
    StreamString s;
    GetDescription(&s, lldb::eDescriptionLevelVerbose);
    LLDB_LOGF(log, "Hit breakpoint location: %s, %s.\n", s.GetData(),
              should_stop ? "stopping" : "continuing");
    if (facade_loc_sp) {
      s.Clear();
      facade_loc_sp->GetDescription(&s, lldb::eDescriptionLevelVerbose);
      LLDB_LOGF(log, "Attributing to facade location: %s.\n", s.GetData());
    }
  }

  return should_stop;
}

void BreakpointLocation::BumpHitCount() {
  if (IsEnabled()) {
    // Step our hit count, and also step the hit count of the owner.
    m_hit_counter.Increment();
    m_owner.m_hit_counter.Increment();
  }
}

void BreakpointLocation::UndoBumpHitCount() {
  if (IsEnabled()) {
    // Step our hit count, and also step the hit count of the owner.
    m_hit_counter.Decrement();
    m_owner.m_hit_counter.Decrement();
  }
}

bool BreakpointLocation::IsResolved() const {

  bool has_site = m_bp_site_sp.get() != nullptr;
  // Facade locations are currently always considered resolved.
  return has_site || IsFacade();
}

lldb::BreakpointSiteSP BreakpointLocation::GetBreakpointSite() const {
  return m_bp_site_sp;
}

llvm::Error BreakpointLocation::ResolveBreakpointSite() {
  // This might be a facade location, which doesn't have an address.
  // In that case, don't attempt to make a site.
  if (m_bp_site_sp || IsFacade())
    return llvm::Error::success();

  // A patched copy's line table points back at the original source, so a
  // breakpoint whose condition was compiled into the process resolves into the
  // copy as well. Arming that location would stop on every pass and evaluate
  // the condition at the stop, which is the cost compiling it in removed. The
  // trap the copy already contains is what reports this breakpoint's hits.
  if (m_owner.HitsComeFromCompiledCode() && IsInDebuggerCompiledCode(m_address))
    return llvm::createStringError(
        "not arming a location in the copy that carries this breakpoint's "
        "compiled-in condition");

  Process *process = m_owner.GetTarget().GetProcessSP().get();
  if (process == nullptr)
    return llvm::createStringError("no process");

  lldb::break_id_t new_id =
      process->CreateBreakpointSite(shared_from_this(), m_owner.IsHardware());

  if (new_id == LLDB_INVALID_BREAK_ID)
    return llvm::createStringError(
        llvm::formatv("Failed to add breakpoint site at {0:x}",
                      m_address.GetOpcodeLoadAddress(&m_owner.GetTarget())));

  if (!IsResolved())
    return llvm::createStringError(
        "breakpoint site created but location is still unresolved");

  return llvm::Error::success();
}

bool BreakpointLocation::SetBreakpointSite(BreakpointSiteSP &bp_site_sp) {
  m_bp_site_sp = bp_site_sp;
  SendBreakpointLocationChangedEvent(eBreakpointEventTypeLocationsResolved);
  return true;
}

llvm::Error BreakpointLocation::ClearBreakpointSite() {
  if (!m_bp_site_sp) {
    // This might be a Facade Location, which don't have sites or addresses
    if (IsFacade())
      return llvm::Error::success();
    return llvm::createStringError("no breakpoint site to clear");
  }

  // If the process exists, get it to remove the owner, it will remove the
  // physical implementation of the breakpoint as well if there are no more
  // owners.  Otherwise just remove this owner.
  if (ProcessSP process_sp = m_owner.GetTarget().GetProcessSP())
    process_sp->RemoveConstituentFromBreakpointSite(GetBreakpoint().GetID(),
                                                    GetID(), m_bp_site_sp);
  else
    m_bp_site_sp->RemoveConstituent(GetBreakpoint().GetID(), GetID());

  m_bp_site_sp.reset();
  return llvm::Error::success();
}

void BreakpointLocation::GetDescription(Stream *s,
                                        lldb::DescriptionLevel level) {
  SymbolContext sc;

  // If this is a scripted breakpoint, give it a chance to describe its
  // locations:
  std::optional<std::string> scripted_opt;
  BreakpointResolverSP resolver_sp = GetBreakpoint().GetResolver();
  BreakpointResolverScripted *scripted =
      llvm::dyn_cast<BreakpointResolverScripted>(resolver_sp.get());
  if (scripted)
    scripted_opt = scripted->GetLocationDescription(shared_from_this(), level);

  bool is_scripted_desc = scripted_opt.has_value();

  // If the description level is "initial" then the breakpoint is printing out
  // our initial state, and we should let it decide how it wants to print our
  // label.
  if (level != eDescriptionLevelInitial) {
    s->Indent();
    BreakpointID::GetCanonicalReference(s, m_owner.GetID(), GetID());
  }

  if (level == lldb::eDescriptionLevelBrief)
    return;

  if (level != eDescriptionLevelInitial)
    s->PutCString(": ");

  if (level == lldb::eDescriptionLevelVerbose)
    s->IndentMore();

  if (is_scripted_desc) {
    s->PutCString(scripted_opt->c_str());
  } else if (m_address.IsSectionOffset()) {
    m_address.CalculateSymbolContext(&sc);

    if (level == lldb::eDescriptionLevelFull ||
        level == eDescriptionLevelInitial) {
      if (IsReExported())
        s->PutCString("re-exported target = ");
      else
        s->PutCString("where = ");

      // If there's a preferred line entry for printing, use that.
      bool show_function_info = true;
      if (auto preferred = GetPreferredLineEntry()) {
        sc.line_entry = *preferred;
        // FIXME: We're going to get the function name wrong when the preferred
        // line entry is not the lowest one.  For now, just leave the function
        // out in this case, but we really should also figure out how to easily
        // fake the function name here.
        show_function_info = false;
      }
      sc.DumpStopContext(s, m_owner.GetTarget().GetProcessSP().get(), m_address,
                         false, true, false, show_function_info,
                         show_function_info, show_function_info);
    } else {
      if (sc.module_sp) {
        s->EOL();
        s->Indent("module = ");
        sc.module_sp->GetFileSpec().Dump(s->AsRawOstream());
      }

      if (sc.comp_unit != nullptr) {
        s->EOL();
        s->Indent("compile unit = ");
        s->PutCString(sc.comp_unit->GetPrimaryFile().GetFilename());

        if (sc.function != nullptr) {
          s->EOL();
          s->Indent("function = ");
          s->PutCString(sc.function->GetName().AsCString("<unknown>"));
          if (ConstString mangled_name =
                  sc.function->GetMangled().GetMangledName()) {
            s->EOL();
            s->Indent("mangled function = ");
            s->PutCString(mangled_name);
          }
        }

        if (sc.line_entry.line > 0) {
          s->EOL();
          s->Indent("location = ");
          if (auto preferred = GetPreferredLineEntry())
            preferred->DumpStopContext(s, true);
          else
            sc.line_entry.DumpStopContext(s, true);
        }

      } else {
        // If we don't have a comp unit, see if we have a symbol we can print.
        if (sc.symbol) {
          s->EOL();
          if (IsReExported())
            s->Indent("re-exported target = ");
          else
            s->Indent("symbol = ");
          s->PutCString(sc.symbol->GetName().AsCString("<unknown>"));
        }
      }
    }
  }

  if (level == lldb::eDescriptionLevelVerbose) {
    s->EOL();
    s->Indent();
  }

  if (!is_scripted_desc) {
    if (m_address.IsSectionOffset() &&
        (level == eDescriptionLevelFull || level == eDescriptionLevelInitial))
      s->PutCString(", ");
    s->PutCString("address = ");

    ExecutionContextScope *exe_scope = nullptr;
    Target *target = &m_owner.GetTarget();
    if (target)
      exe_scope = target->GetProcessSP().get();
    if (exe_scope == nullptr)
      exe_scope = target;

    if (level == eDescriptionLevelInitial)
      m_address.Dump(s, exe_scope, Address::DumpStyleLoadAddress,
                     Address::DumpStyleFileAddress);
    else
      m_address.Dump(s, exe_scope, Address::DumpStyleLoadAddress,
                     Address::DumpStyleModuleWithFileAddress);

    if (IsIndirect() && m_bp_site_sp) {
      Address resolved_address;
      resolved_address.SetLoadAddress(m_bp_site_sp->GetLoadAddress(), target);
      const Symbol *resolved_symbol =
          resolved_address.CalculateSymbolContextSymbol();
      if (resolved_symbol) {
        if (level == eDescriptionLevelFull || level == eDescriptionLevelInitial)
          s->PutCString(", ");
        else if (level == lldb::eDescriptionLevelVerbose) {
          s->EOL();
          s->Indent();
        }
        s->Printf("indirect target = %s",
                  resolved_symbol->GetName().GetCString());
      }
    }
  }

  // FIXME: scripted breakpoint are currently always resolved.  Does this seem
  // right? If they don't add any scripted locations, we shouldn't consider them
  // resolved.
  bool is_resolved = is_scripted_desc || IsResolved();
  // A scripted breakpoint might be resolved but not have a site.  Be sure to
  // check for that.
  bool is_hardware = !is_scripted_desc && IsResolved() && m_bp_site_sp &&
                     m_bp_site_sp->IsHardware();

  if (level == lldb::eDescriptionLevelVerbose) {
    s->EOL();
    s->Indent();
    s->Printf("resolved = %s\n", is_resolved ? "true" : "false");
    s->Indent();
    s->Printf("hardware = %s\n", is_hardware ? "true" : "false");
    s->Indent();
    s->Printf("hit count = %-4u\n", GetHitCount());

    if (m_options_up) {
      s->Indent();
      m_options_up->GetDescription(s, level);
      s->EOL();
    }
    s->IndentLess();
  } else if (level != eDescriptionLevelInitial) {
    s->Printf(", %sresolved, %shit count = %u ", (is_resolved ? "" : "un"),
              (is_hardware ? "hardware, " : ""), GetHitCount());
    if (m_options_up) {
      m_options_up->GetDescription(s, level);
    }
  }
}

void BreakpointLocation::Dump(Stream *s) const {
  if (s == nullptr)
    return;

  bool is_resolved = IsResolved();
  bool is_hardware = is_resolved && m_bp_site_sp->IsHardware();

  lldb::tid_t tid = GetOptionsSpecifyingKind(BreakpointOptions::eThreadSpec)
                        .GetThreadSpecNoCreate()
                        ->GetTID();
  s->Printf("BreakpointLocation %u: tid = %4.4" PRIx64
            "  load addr = 0x%8.8" PRIx64 "  state = %s  type = %s breakpoint  "
            "hit_count = %-4u  ignore_count = %-4u",
            GetID(), tid,
            (uint64_t)m_address.GetOpcodeLoadAddress(&m_owner.GetTarget()),
            (m_options_up ? m_options_up->IsEnabled() : m_owner.IsEnabled())
                ? "enabled "
                : "disabled",
            is_hardware ? "hardware" : "software", GetHitCount(),
            GetOptionsSpecifyingKind(BreakpointOptions::eIgnoreCount)
                .GetIgnoreCount());
}

void BreakpointLocation::SendBreakpointLocationChangedEvent(
    lldb::BreakpointEventType eventKind) {
  if (!m_owner.IsInternal()) {
    auto data_sp = std::make_shared<Breakpoint::BreakpointEventData>(
        eventKind, m_owner.shared_from_this());
    data_sp->GetBreakpointLocationCollection().Add(shared_from_this());
    m_owner.GetTarget().NotifyBreakpointChanged(m_owner, data_sp);
  }
}

std::optional<uint32_t> BreakpointLocation::GetSuggestedStackFrameIndex() {
  auto preferred_opt = GetPreferredLineEntry();
  if (!preferred_opt)
    return {};
  LineEntry preferred = *preferred_opt;
  SymbolContext sc;
  if (!m_address.CalculateSymbolContext(&sc))
    return {};
  // Don't return anything special if frame 0 is the preferred line entry.
  // We not really telling the stack frame list to do anything special in that
  // case.
  if (!LineEntry::Compare(sc.line_entry, preferred))
    return {};

  if (!sc.block)
    return {};

  // Blocks have their line info in Declaration form, so make one here:
  Declaration preferred_decl(preferred.GetFile(), preferred.line,
                             preferred.column);

  uint32_t depth = 0;
  Block *inlined_block = sc.block->GetContainingInlinedBlock();
  while (inlined_block) {
    // If we've moved to a block that this isn't the start of, that's not
    // our inlining info or call site, so we can stop here.
    Address start_address;
    if (!inlined_block->GetStartAddress(start_address) ||
        start_address != m_address)
      return {};

    const InlineFunctionInfo *info = inlined_block->GetInlinedFunctionInfo();
    if (info) {
      if (preferred_decl == info->GetDeclaration())
        return depth;
      if (preferred_decl == info->GetCallSite())
        return depth + 1;
    }
    inlined_block = inlined_block->GetInlinedParent();
    depth++;
  }
  return {};
}

void BreakpointLocation::SwapLocation(BreakpointLocationSP swap_from) {
  m_address = swap_from->m_address;
  m_should_resolve_indirect_functions =
      swap_from->m_should_resolve_indirect_functions;
  m_is_reexported = swap_from->m_is_reexported;
  m_is_indirect = swap_from->m_is_indirect;
  m_user_expression_sp.reset();
}

void BreakpointLocation::SetThreadIDInternal(lldb::tid_t thread_id) {
  if (thread_id != LLDB_INVALID_THREAD_ID) {
    GetLocationOptions().SetThreadID(thread_id);
  } else {
    // If we're resetting this to an invalid thread id, then don't make an
    // options pointer just to do that.
    if (m_options_up != nullptr)
      m_options_up->SetThreadID(thread_id);
  }
}
