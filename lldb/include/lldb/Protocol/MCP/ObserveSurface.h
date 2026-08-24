//===-- ObserveSurface.h --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The client-facing surface of the `observe` tool: what a caller reads about it
// and the shape of the plan it accepts.
//
// It lives here, in a header both can include, because two servers present the
// same tool. `lldb-mcp` answers tools/list itself rather than reaching into the
// plugin that implements the tool, and an LLDB with a running MCP server serves
// it from that plugin. Those were two copies of the same eighteen hundred
// characters, kept in step by a comment saying so, and the moment they were
// edited is exactly the moment that goes wrong.
//
// Only the `debugger` argument differs between the two, because a client sees
// instance-qualified URIs and a backend does not, so that one field is supplied
// by each server rather than shared.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_PROTOCOL_MCP_OBSERVESURFACE_H
#define LLDB_PROTOCOL_MCP_OBSERVESURFACE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include <cstdint>
#include <utility>

namespace lldb_protocol::mcp {

/// What the `observe` tool is for, in the terms a caller cannot infer from the
/// schema.
///
/// It opens with a plan rather than with prose. Measured on two agents given
/// this tool on two different tasks: both reported that they could not tell what
/// a tracepoint looked like, one of them abandoned runtime inspection after a
/// single call over it, and both had the whole schema in front of them the entire
/// time -- one of them having already used three of its fields correctly. A
/// nested schema is read, and an example is understood, so the example is what
/// leads.
inline constexpr llvm::StringLiteral ObserveToolDescription =
    "Run a program under a set of tracepoints and report what happened, "
    "instead of stepping through it. One call launches the program, reads the "
    "expressions named at each tracepoint, lets the program run to its own "
    "end, and comes back with a summary over every hit plus a JSONL artifact "
    "holding the full event stream.\n"
    "\n"
    "A plan with two tracepoints on it:\n"
    "{\"plan\": {\"program\": \"build/bin/opt\",\n"
    "           \"args\": [\"-passes=sroa\", \"-S\", \"in.ll\"],\n"
    "           \"observe\": [\n"
    "             {\"at\": \"SSAUpdater.cpp:450\",\n"
    "              \"capture\": [\"L.Name\", \"StoredValue.Ty.TypeID\"]},\n"
    "             {\"at\": \"llvm::SROA::runOnAlloca\", \"on\": \"return\",\n"
    "              \"capture\": [\"$return\"], \"emit\": \"on_change\"}]}}\n"
    "\n"
    "An empty \"observe\" list is crash triage: the program runs untouched, "
    "and the result is how it ended, with a ranked backtrace, locals and "
    "source at the failure. For a program that is stuck rather than crashing, "
    "that same plan reports \"profile\" -- where sampled stacks found it, which "
    "answers what is running without being told where to look.\n"
    "\n"
    "Prefer a capture that is a path, \"I.Ty.TypeID\", over one that is a "
    "call, \"I->getType()\"; \"->\" and \"[]\" are part of a path. A path is a "
    "debug-info lookup and a memory read, while a call compiles and runs code "
    "inside the observed process, and on a hot tracepoint a call is measured "
    "and turned off partway through the run.\n"
    "\n"
    "Capture more expressions than you think you need. A capture costs wall "
    "clock once per run, not tokens per round trip, and the alternative to "
    "capturing it now is running the whole program again to ask one more "
    "question.\n"
    "\n"
    "Read \"aggregate\" first. Its \"outliers\", the values seen once or twice "
    "among many hits, are usually the answer. Then re-run with \"only_hit\" "
    "set to that outlier's \"first_hit\", which records that one hit in full "
    "detail: captures are read there alone, so a call is affordable, and what "
    "one prints comes back in \"inferior_output\".";

/// The schema of one entry in the plan's "observe" list.
llvm::json::Value ObservationSchema();

/// The schema of the plan, which is everything describing the run. Nested under
/// "plan" rather than flattened into the tool's arguments because its fields are
/// validated as a closed set, and admitting a non-plan field at that level would
/// make the error that lists the accepted ones wrong.
llvm::json::Value ObservationPlanSchema();

} // namespace lldb_protocol::mcp

#endif
