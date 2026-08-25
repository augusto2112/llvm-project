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
inline constexpr llvm::StringLiteral ObserveToolDescription =
    "Run a program under a set of tracepoints and report what happened, "
    "instead of stepping through it. One call launches the program, reads the "
    "expressions named at each tracepoint, lets the program run to its own "
    "end, and comes back with a summary over every hit plus a JSONL artifact "
    "holding the full event stream.\n"
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
    "An \"at\" is a function name, qualified or not, or a source location "
    "written as \"file.cpp:1189\".\n"
    "\n"
    "An empty \"observe\" list is crash triage: the program runs untouched, "
    "and the result is how it ended, with a ranked backtrace, locals and "
    "source at the failure. For a program that is stuck rather than crashing, "
    "that same plan reports \"profile\" -- where sampled stacks found it, which "
    "answers what is running without being told where to look.\n"
    "\n"
    "What matters about a capture is whether it is a path or a call, not how "
    "it is spelled: \"tok.kind\" and \"tok->kind\" are both paths, and "
    "\"->\" and \"[]\" are as much a part of one as \".\" is. Prefer a path "
    "over a call, \"tok->describe()\". A path is a debug-info lookup and a "
    "memory read, while a call compiles and runs code inside the observed "
    "process, and on a hot tracepoint a call is measured and turned off "
    "partway through the run.\n"
    "\n"
    "A path names what the type declares, which is not what it exposes: where a "
    "class offers \"getName()\", the path is the field that accessor reads, "
    "private or not, because debug info describes storage rather than an API. "
    "Reading a field name off an accessor is the common way a capture fails, so "
    "where a name is a guess, capture the accessor call as well.\n"
    "\n"
    "A value with a custom rendering needs a data formatter to get it, and "
    "formatters are not built in: this session loads what \"~/.lldbinit\" "
    "imports, and without them a value comes back expanded into its members "
    "instead. A run that met no formatter at all says so in \"notes\", so an "
    "expanded struct is not left looking like the value the program holds.\n"
    "\n"
    "Capture more expressions than you think you need. A capture costs wall "
    "clock once per run, not tokens per round trip, and the alternative to "
    "capturing it now is running the whole program again to ask one more "
    "question. A wrong guess is cheap: a capture that cannot resolve is "
    "stopped at its first hit and reported in \"capture_failures\" with the "
    "compiler's reason and the names that were in scope, so read that array "
    "before concluding a value was uninteresting.\n"
    "\n"
    "To answer whether a change moved anything, put the runs in \"compare\" and "
    "get back the differences rather than two reports to diff: [{\"label\": "
    "\"fixed\"}, {\"label\": \"before\", \"program\": \"/tmp/opt.before\"}] runs "
    "the same tracepoints over both binaries and reports how each ended, what "
    "disagreed, the first hit at which they stopped agreeing, and the names of "
    "everything that matched.\n"
    "\n"
    "Read \"aggregate\" first. Its \"outliers\", the values seen once or twice "
    "among many hits, are usually the answer. Then re-run with \"only_hit\" "
    "set to that outlier's \"first_hit\", which records that one hit in full "
    "detail: captures are read there alone, so a call is affordable.\n"
    "\n"
    "A capture that prints rather than returning a value -- \"tok->dump()\" -- "
    "comes back with what it printed beside it, in \"printed\", per hit and with "
    "\"stdout\" and \"stderr\" apart. That text is part of the capture's value, so "
    "\"emit\": \"on_change\" over a printer emits when what it prints changes. "
    "The program's own output is reported separately in \"inferior_output\". "
    "Attribution is best-effort: text the program had written and not yet "
    "flushed when a capture ran is credited to that capture, and a capture that "
    "prints for the first time may have that hit's text reported as the "
    "program's.";

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

} // namespace lldb_protocol::mcp

#endif
