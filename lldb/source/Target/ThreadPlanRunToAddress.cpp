//===-- ThreadPlanRunToAddress.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/ThreadPlanRunToAddress.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Stream.h"

using namespace lldb;
using namespace lldb_private;

// ThreadPlanRunToAddress: Continue plan

ThreadPlanRunToAddress::ThreadPlanRunToAddress(Thread &thread, Address &address,
                                               bool stop_others)
    : ThreadPlan(ThreadPlan::eKindRunToAddress, "Run to address plan", thread,
                 eVoteNoOpinion, eVoteNoOpinion),
      m_stop_others(stop_others), m_addresses(), m_break_ids() {
  m_addresses.push_back(
      address.GetOpcodeLoadAddress(thread.CalculateTarget().get()));
  SetInitialBreakpoints();
}

ThreadPlanRunToAddress::ThreadPlanRunToAddress(Thread &thread,
                                               lldb::addr_t address,
                                               bool stop_others)
    : ThreadPlan(ThreadPlan::eKindRunToAddress, "Run to address plan", thread,
                 eVoteNoOpinion, eVoteNoOpinion),
      m_stop_others(stop_others), m_addresses(), m_break_ids() {
  m_addresses.push_back(
      thread.CalculateTarget()->GetOpcodeLoadAddress(address));
  SetInitialBreakpoints();
}

ThreadPlanRunToAddress::ThreadPlanRunToAddress(
    Thread &thread, const std::vector<lldb::addr_t> &addresses,
    bool stop_others)
    : ThreadPlan(ThreadPlan::eKindRunToAddress, "Run to address plan", thread,
                 eVoteNoOpinion, eVoteNoOpinion),
      m_stop_others(stop_others), m_addresses(addresses), m_break_ids() {
  // Convert all addresses into opcode addresses to make sure we set
  // breakpoints at the correct address.
  Target &target = thread.GetProcess()->GetTarget();
  std::vector<lldb::addr_t>::iterator pos, end = m_addresses.end();
  for (pos = m_addresses.begin(); pos != end; ++pos)
    *pos = target.GetOpcodeLoadAddress(*pos);

  SetInitialBreakpoints();
}

void ThreadPlanRunToAddress::SetInitialBreakpoints() {
  size_t num_addresses = m_addresses.size();
  m_break_ids.resize(num_addresses);

  for (size_t i = 0; i < num_addresses; i++) {
    Breakpoint *breakpoint;
    breakpoint =
        GetTarget().CreateBreakpoint(m_addresses[i], true, false).get();
    if (breakpoint != nullptr) {
      if (breakpoint->IsHardware() && !breakpoint->HasResolvedLocations())
        m_could_not_resolve_hw_bp = true;
      m_break_ids[i] = breakpoint->GetID();
      breakpoint->SetThreadID(m_tid);
      breakpoint->SetBreakpointKind("run-to-address");
    }
  }
}

ThreadPlanRunToAddress::~ThreadPlanRunToAddress() {
  size_t num_break_ids = m_break_ids.size();
  for (size_t i = 0; i < num_break_ids; i++) {
    GetTarget().RemoveBreakpointByID(m_break_ids[i]);
  }
  m_could_not_resolve_hw_bp = false;
}

void ThreadPlanRunToAddress::GetDescription(Stream *s,
                                            lldb::DescriptionLevel level) {
  size_t num_addresses = m_addresses.size();

  if (level == lldb::eDescriptionLevelBrief) {
    if (num_addresses == 0) {
      s->Printf("run to address with no addresses given.");
      return;
    } else if (num_addresses == 1)
      s->Printf("run to address: ");
    else
      s->Printf("run to addresses: ");

    for (size_t i = 0; i < num_addresses; i++) {
      DumpAddress(s->AsRawOstream(), m_addresses[i], sizeof(addr_t));
      s->Printf(" ");
    }
  } else {
    if (num_addresses == 0) {
      s->Printf("run to address with no addresses given.");
      return;
    } else if (num_addresses == 1)
      s->Printf("Run to address: ");
    else {
      s->Printf("Run to addresses: ");
    }

    for (size_t i = 0; i < num_addresses; i++) {
      if (num_addresses > 1) {
        s->Printf("\n");
        s->Indent();
      }

      DumpAddress(s->AsRawOstream(), m_addresses[i], sizeof(addr_t));
      s->Printf(" using breakpoint: %d - ", m_break_ids[i]);
      Breakpoint *breakpoint =
          GetTarget().GetBreakpointByID(m_break_ids[i]).get();
      if (breakpoint)
        breakpoint->Dump(s);
      else
        s->Printf("but the breakpoint has been deleted.");
    }
  }
}

bool ThreadPlanRunToAddress::ValidatePlan(Stream *error) {
  if (m_could_not_resolve_hw_bp) {
    if (error)
      error->Printf("Could not set hardware breakpoint(s)");
    return false;
  }

  // If we couldn't set the breakpoint for some reason, then this won't work.
  bool all_bps_good = true;
  size_t num_break_ids = m_break_ids.size();
  for (size_t i = 0; i < num_break_ids; i++) {
    if (m_break_ids[i] == LLDB_INVALID_BREAK_ID) {
      all_bps_good = false;
      if (error) {
        error->Printf("Could not set breakpoint for address: ");
        DumpAddress(error->AsRawOstream(), m_addresses[i], sizeof(addr_t));
        error->Printf("\n");
      }
    }
  }
  return all_bps_good;
}

bool ThreadPlanRunToAddress::DoPlanExplainsStop(Event *event_ptr) {
  return AtOurAddress();
}

bool ThreadPlanRunToAddress::ShouldStop(Event *event_ptr) {
  return AtOurAddress();
}

bool ThreadPlanRunToAddress::StopOthers() { return m_stop_others; }

void ThreadPlanRunToAddress::SetStopOthers(bool new_value) {
  m_stop_others = new_value;
}

StateType ThreadPlanRunToAddress::GetPlanRunState() { return eStateRunning; }

bool ThreadPlanRunToAddress::WillStop() { return true; }

bool ThreadPlanRunToAddress::MischiefManaged() {
  Log *log = GetLog(LLDBLog::Step);

  if (AtOurAddress()) {
    // Remove the breakpoint
    size_t num_break_ids = m_break_ids.size();

    for (size_t i = 0; i < num_break_ids; i++) {
      if (m_break_ids[i] != LLDB_INVALID_BREAK_ID) {
        GetTarget().RemoveBreakpointByID(m_break_ids[i]);
        m_break_ids[i] = LLDB_INVALID_BREAK_ID;
      }
    }
    LLDB_LOGF(log, "Completed run to address plan.");
    ThreadPlan::MischiefManaged();
    return true;
  } else
    return false;
}

bool ThreadPlanRunToAddress::AtOurAddress() {
  lldb::addr_t current_address = GetThread().GetRegisterContext()->GetPC();
  bool found_it = false;
  size_t num_addresses = m_addresses.size();
  for (size_t i = 0; i < num_addresses; i++) {
    if (m_addresses[i] == current_address) {
      found_it = true;
      break;
    }
  }
  return found_it;
}

ThreadPlanSP ThreadPlanRunToAddress::MakeThreadPlanRunToAddressFromSymbol(
    Thread &thread, ConstString symbol_name, bool stop_others) {
  TargetSP target_sp(thread.CalculateTarget());
  if (!target_sp)
    return {};

  Log *log = GetLog(LLDBLog::Step);
  std::vector<Address> addresses;
  const ModuleList &images = target_sp->GetImages();

  SymbolContextList code_symbols;
  images.FindSymbolsWithNameAndType(symbol_name, eSymbolTypeCode, code_symbols);
  size_t num_code_symbols = code_symbols.GetSize();

  if (num_code_symbols > 0) {
    for (uint32_t i = 0; i < num_code_symbols; i++) {
      SymbolContext context;
      AddressRange addr_range;
      if (code_symbols.GetContextAtIndex(i, context)) {
        context.GetAddressRange(eSymbolContextEverything, 0, false, addr_range);
        addresses.push_back(addr_range.GetBaseAddress());
        if (log) {
          addr_t load_addr =
              addr_range.GetBaseAddress().GetLoadAddress(target_sp.get());

          LLDB_LOGF(log, "Found a trampoline target symbol at 0x%" PRIx64 ".",
                    load_addr);
        }
      }
    }
  }

  SymbolContextList reexported_symbols;
  images.FindSymbolsWithNameAndType(symbol_name, eSymbolTypeReExported,
                                    reexported_symbols);
  size_t num_reexported_symbols = reexported_symbols.GetSize();
  if (num_reexported_symbols > 0) {
    for (uint32_t i = 0; i < num_reexported_symbols; i++) {
      SymbolContext context;
      if (reexported_symbols.GetContextAtIndex(i, context)) {
        if (context.symbol) {
          Symbol *actual_symbol =
              context.symbol->ResolveReExportedSymbol(*target_sp.get());
          if (actual_symbol) {
            const Address actual_symbol_addr = actual_symbol->GetAddress();
            if (actual_symbol_addr.IsValid()) {
              addresses.push_back(actual_symbol_addr);
              if (log) {
                lldb::addr_t load_addr =
                    actual_symbol_addr.GetLoadAddress(target_sp.get());
                LLDB_LOGF(log,
                          "Found a re-exported symbol: %s at 0x%" PRIx64 ".",
                          actual_symbol->GetName().GetCString(), load_addr);
              }
            }
          }
        }
      }
    }
  }

  SymbolContextList indirect_symbols;
  images.FindSymbolsWithNameAndType(symbol_name, eSymbolTypeResolver,
                                    indirect_symbols);
  size_t num_indirect_symbols = indirect_symbols.GetSize();
  if (num_indirect_symbols > 0) {
    for (uint32_t i = 0; i < num_indirect_symbols; i++) {
      SymbolContext context;
      AddressRange addr_range;
      if (indirect_symbols.GetContextAtIndex(i, context)) {
        context.GetAddressRange(eSymbolContextEverything, 0, false, addr_range);
        addresses.push_back(addr_range.GetBaseAddress());
        if (log) {
          addr_t load_addr =
              addr_range.GetBaseAddress().GetLoadAddress(target_sp.get());

          LLDB_LOGF(log, "Found an indirect target symbol at 0x%" PRIx64 ".",
                    load_addr);
        }
      }
    }
  }

  if (addresses.empty())
    return {};

  // First check whether any of the addresses point to Indirect symbols,
  // and if they do, resolve them:
  std::vector<lldb::addr_t> load_addrs;
  for (Address address : addresses) {
    Symbol *symbol = address.CalculateSymbolContextSymbol();
    if (symbol && symbol->IsIndirect()) {
      Status error;
      Address symbol_address = symbol->GetAddress();
      addr_t resolved_addr =
          thread.GetProcess()->ResolveIndirectFunction(&symbol_address, error);
      if (error.Success()) {
        load_addrs.push_back(resolved_addr);
      }
    } else {
      load_addrs.push_back(address.GetLoadAddress(target_sp.get()));
    }
  }

  return std::make_shared<ThreadPlanRunToAddress>(thread, load_addrs,
                                                  stop_others);
}

