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
// `command` is served by the plugin alone: `lldb-mcp` advertises `trace_program`
// and nothing else, so its description is shared with no one and sits here only
// beside the text that explains the boundary between the two.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_PROTOCOL_MCP_OBSERVESURFACE_H
#define LLDB_PROTOCOL_MCP_OBSERVESURFACE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include <cstdint>
#include <utility>

namespace lldb_protocol::mcp {

/// What the `trace_program` tool is for, in the terms a caller cannot infer from
/// the schema.
///
/// The name carries more of this than the text does. Measured on an agent given a
/// hang to fix: the tools reached it as names in a deferred list with their schemas
/// unloaded, it judged "from its names alone" that a debugger was a poor fit for a
/// nonterminating pass, and it never spent the call that would have shown it any of
/// what follows. A name that says the tool runs a program and traces it is
/// therefore load-bearing in a way a description cannot be, because the description
/// is read after the decision it would have informed.
///
/// The opening sentence is the next thing to get right, and for the same reason:
/// where only the name and the head of this string arrive, they are the entire
/// discovery surface. So the first two sentences say what the tool does *and* that
/// the argument is a plan naming functions and expressions -- the one thing 23
/// recorded write-ups got wrong, calling `plan` an opaque JSON string, 16 of them
/// never placing a tracepoint at all.
///
/// It opens with a plan rather than with prose. Measured on two agents given
/// this tool on two different tasks: both reported that they could not tell what
/// a tracepoint looked like, one of them abandoned runtime inspection after a
/// single call over it, and both had the whole schema in front of them the entire
/// time -- one of them having already used three of its fields correctly. A
/// nested schema is read, and an example is understood, so the example is what
/// leads.
///
/// Which makes the example the most load-bearing string here, and it has to be
/// right about two things a caller cannot check. It is copied for its *shape*,
/// so a capture spelled in a way that does not resolve is a failure handed to
/// everyone who copies it: an earlier example read a field off a class where the
/// real declaration had none, and the captures agents wrote afterwards imitated
/// it name for name, drawing "no member named" from the compiler each time. And
/// an example naming real code names it for every caller, whatever they are
/// working on, so it stays away from code anyone might be sent here to fix.
///
/// Both are settled the same way: the example names the program the observe API
/// test builds, and that test runs the example out of this string as it is
/// written, replacing "program" and nothing else. A spelling that stops
/// resolving fails a test rather than a caller.
///
/// What is *not* here came out on evidence that length was not buying
/// comprehension. This string was 3,930 characters and the schema beside it
/// 6,352 wire bytes -- typed, closed with `additionalProperties: false`, an enum
/// and a default on every field that has one -- and across 47 recorded runs none
/// of it arrived: the tools reached the agent as bare names in a deferred list,
/// 36 attempts to recover a schema each came back with 209 characters and none of
/// them got it, and 23 write-ups afterwards describe `plan` as an opaque JSON
/// string, 16 of those never placing a tracepoint at all. So the name and the
/// opening sentence carry the discovery on their own, and are worth more care
/// than everything behind them; volume is not the lever it looks like.
///
/// For the client that does receive all of it -- an editor, or any client that
/// lists a tool before calling it -- the argument for cutting is legibility and
/// one outright defect. Three paragraphs said the same sentence as the schema
/// field they explained, 824 characters of exact duplication: what an "at" may
/// be, that a path names what a type declares rather than what it exposes, and
/// how "compare" reports. The schema's copy is the one read at the moment a
/// caller sets that field, and two copies of a sentence are two things to keep
/// true. Two more paragraphs came out at a real loss and are covered elsewhere:
/// data formatters, which the engine already pushes a note about when a run met
/// none, and the attribution of printed output, which the response labels for
/// itself.
///
/// What stays is what a caller cannot be told by the field it is filling in:
/// that this tool exists instead of stepping, what an empty plan does, which
/// field of the response to read first, and the one economic argument -- capture
/// generously -- that a schema has no place to make.
inline constexpr llvm::StringLiteral ObserveToolDescription =
    "Run a program under a set of tracepoints and report what happened, "
    "instead of stepping through it. The \"plan\" argument names the functions "
    "to stop at and the expressions to read at each; one call launches the "
    "program, reads them, lets it run to its own end, and returns a summary "
    "over every hit plus a JSONL artifact holding the full event stream.\n"
    "\n"
    "A plan with two tracepoints on it:\n"
    "{\"plan\": {\"program\": \"build/bin/analyze\",\n"
    "           \"args\": [\"input.txt\"],\n"
    "           \"observe\": [\n"
    "             {\"at\": \"classify_token\",\n"
    "              \"capture\": [\"tok->kind\", \"tok->text\", \"depth\"]},\n"
    "             {\"at\": \"parse_expr\", \"on\": \"return\",\n"
    "              \"capture\": [\"$return\"], \"emit\": \"on_change\"}]}}\n"
    "\n"
    "An empty \"observe\" list is crash triage: the program runs untouched and "
    "the result is how it ended, with a ranked backtrace, locals and source. "
    "For a program that is stuck rather than crashing the same plan reports "
    "\"profile\" -- where sampled stacks found it -- which answers what is "
    "running without being told where to look.\n"
    "\n"
    "Read \"aggregate\" first: its \"outliers\", the values seen once or twice "
    "among many hits, are usually the answer. Re-run with \"only_hit\" set to "
    "that outlier's \"first_hit\" to record that one hit in full detail.\n"
    "\n"
    "Capture generously, and prefer a path (\"tok->kind\") to a call "
    "(\"tok->describe()\"): a capture costs wall clock once per run rather than "
    "tokens per turn, and one that cannot resolve is reported in "
    "\"capture_failures\" with the names that were in scope.";

/// What the `command` tool is for, and how it divides the work with
/// `trace_program`.
///
/// Three agents in a row reported that they could not tell which of the two to
/// reach for; one of them never called `command` at all, saying it could not work
/// out whether there was a stopped process for it to act on. The boundary is what
/// each one owns: a command acts on the session as it is, and an observation owns a
/// run from launch to exit.
inline constexpr llvm::StringLiteral CommandToolDescription =
    "Run one LLDB command in a debug session and return its output, the same "
    "text the LLDB command interpreter would print.\n"
    "\n"
    "This is the interactive surface. It acts on whatever state the session is "
    "already in and leaves that state behind for the next call, so it is what to "
    "use to look around a process that is stopped, or to set a target up by hand.\n"
    "\n"
    "To run a program and collect state while it runs, use \"trace_program\" "
    "instead: it "
    "does the launch, the tracepoints and the run in one call, and owns the process "
    "it started -- nothing is left stopped for a command to inspect afterwards.";

/// The schema of one entry in the plan's "observe" list.
llvm::json::Value ObservationSchema();

/// The schema of the plan, which is everything describing the run. Nested under
/// "plan" rather than flattened into the tool's arguments because its fields are
/// validated as a closed set, and admitting a non-plan field at that level would
/// make the error that lists the accepted ones wrong.
llvm::json::Value ObservationPlanSchema();

/// The whole `inputSchema` of the tool: the plan, plus the one argument that
/// selects a session.
///
/// \p debugger_description is the only thing the two servers disagree about, so
/// it is a parameter rather than a second copy of the schema. It was two copies:
/// the text a client actually saw lived in `lldb-mcp`, the plugin carried a
/// shorter one that nothing served, and a reader could not tell which was which.
/// Editing the dead one is silent -- the wire is unchanged and no test moves --
/// which is the failure this signature exists to make impossible.
llvm::json::Value ObserveInputSchema(llvm::StringRef debugger_description);

} // namespace lldb_protocol::mcp

#endif
