# Model Context Protocol (MCP)

LLDB supports the [Model Context Protocol](https://modelcontextprotocol.io)
(MCP). This structured, machine-friendly protocol allows AI models to access
and interact with external tools, for example debuggers. Using MCP, an AI agent
can execute LLDB commands to control the debugger: set breakpoints, inspect
memory, step through code. This can range from helping you run a specific
command you cannot immediately remember, to a fully agent-driven debugging
experience.

## Getting Started

LLDB ships with `lldb-mcp`, a binary that speaks MCP over standard input and
output (stdio). Point your MCP client at it, and you are all set.

Configuration example for [Claude Code](https://modelcontextprotocol.io/quickstart/user):

```
claude mcp add lldb --transport stdio -- /path/to/lldb-mcp
```

Configuration example (`mcp.json`) for [Visual Studio Code](https://code.visualstudio.com/docs/copilot/chat/mcp-servers):

```json
{
  "servers": {
    "lldb": {
      "type": "stdio",
      "command": "/path/to/lldb-mcp"
    }
  }
}
```

The MCP client launches one `lldb-mcp` process per connection and shuts it down
when it disconnects, taking any session it created with it.

## Tools

Tools are a primitive in the Model Context Protocol that enable servers to
expose functionality to clients. `lldb-mcp` exposes five.

### `session_create`

Creates a new debug session and returns its URI. This is equivalent to
launching a new instance of `lldb` on the command line. Sessions look like
this:

```
lldb-mcp://instance/{pid}/debugger/{id}
```

The `pid` identifies the process hosting the session and the `id` identifies
the debugger inside it. Pass the whole URI back to the other tools.

### `command`

Runs an LLDB command in a debug session and returns its output, the same text
you would see in the LLDB command interpreter. It takes:

- `command` (required): the command to run, for example `breakpoint set --name main`.
- `debugger` (optional): the URI of the session to run it in. When omitted, the
  command runs in the first session `lldb-mcp` created.

Commands run one at a time and the result comes back when the command finishes.

### `observe`

Runs a program under a set of tracepoints and reports what happened, rather than
stepping through it. One call launches the program, reads the expressions named
at each tracepoint, lets the program run to its own end, and returns a summary
over every hit together with a JSONL file holding the full event stream.

It takes:

- `plan` (required): what to run and what to watch.
- `debugger` (optional): the URI of the session to run it in.

The plan is nested rather than sitting beside `debugger` because its fields are
validated as a closed set: a field the plan does not define is an error, and the
error lists the fields that are accepted. Admitting a non-plan field at that
level would make that list wrong exactly when someone is relying on it.

A plan describes the program:

```json
{
  "plan": {
    "program": "build/bin/opt",
    "args": ["-passes=instcombine", "-S", "repro.ll"],
    "timeout_seconds": 60,
    "observe": [
      {
        "at": "InstCombinerImpl::visitAdd",
        "capture": ["I.Ty.TypeID", "I.hasNSW"],
        "emit": "on_change"
      }
    ]
  }
}
```

`at` is a function name, or a source location written as `file.cpp:1189`. A
trailing colon and digits is what tells the two apart, so a qualified name like
`Foo::bar` keeps its scope operator. An address is refused: it is only
meaningful inside the run that produced it, so in the next run the same number
names a different instruction or none at all.

An observation may also carry `when` (a condition), `called_from` (restrict to
hits reached from another function), `enabled_after` (hold it disabled until
another observation has been hit), `skip_first`, `only_hit`, `backtrace`,
`depth`, and `on` set to `entry` or `return`. `emit` is `every_hit`,
`on_change`, or `first_and_last`.

**An empty `observe` list is crash triage.** The program runs untouched and the
result is how it ended, with a ranked backtrace, locals and source at the
failure. That is the shortest useful plan:

```json
{"plan": {"program": "build/bin/opt", "args": ["-passes=instcombine", "repro.ll"]}}
```

#### Reading the result

The response carries `outcome` — `exited`, `crashed`, `timed_out` or
`no_progress` — a `plan_report` per observation, an `aggregate`, a `terminal`
event, and a pointer to the artifact.

Read `aggregate` first. For each captured expression it gives the distinct
values with counts, the transitions between them, and `outliers`: the values
seen only once or twice among many hits. That last field is usually the answer.
One vector type among four thousand integers, with the hit where it first
appeared, is the bug — computed rather than left to be found. Re-run with
`only_hit` set to that `first_hit` to record that single hit in full detail;
cost does not matter at one hit, so captures and backtrace depth can be as
generous as you like.

An outlier carries two numbers because they count different things.
`first_hit` counts that observation's own hits and is what `only_hit` takes.
`first_seq` numbers the whole event stream and is what matches a line in the
artifact. They are equal only when a plan holds a single observation.

Both `values` and `transitions` are bounded, and report `values_elided` and
`transitions_elided` beside themselves when they drop anything. `distinct`
always counts every value, so a shortened histogram cannot be mistaken for the
real cardinality, and outliers are chosen before the bound applies, so the rare
value that is usually the answer is never the one dropped.

`plan_report` keeps four numbers apart on purpose: how many locations the name
resolved to, how many times the tracepoint was hit, how many of those hits had a
true condition, and how many events were emitted. A misspelled function name, a
condition that never held, and code that never ran all produce no events, and
these numbers are what tell them apart. A name that resolved to nothing comes
back with the nearest names that do exist.

Events themselves live in the artifact, one JSON object per line, and the
response reports its path and field names. The last few events are included
inline only when the program ended badly, which is when they are wanted.

#### Writing a good plan

Prefer a capture written as a path, `I.Ty.TypeID`, over one written as a call,
`I->getType()`. `->` and `[]` are part of a path. A path is a debug-info lookup
and a memory read; a call compiles an expression and runs it inside the observed
process. On a tracepoint hit thousands of times the difference decides whether
the run finishes, and a call that proves too expensive is measured and switched
off partway through so the run stays inside its timeout — yielding partial data
where the path would have yielded all of it. The report says when this happened
and names the cheaper spelling.

Capture more expressions than seems necessary. A capture costs wall clock once
per run, not tokens per exchange, and the alternative to capturing something now
is running the whole program again to ask one more question.

### `sessions_list`

Lists every debug session reachable from this `lldb-mcp`, one URI per line.
That includes sessions it created itself and sessions in LLDB instances running
elsewhere on the machine (see [Attaching to a Running LLDB](#attaching-to-a-running-lldb)).

### `session_close`

Closes a session and frees its resources. It takes a single required `session`
argument, the URI to close. Only sessions that `lldb-mcp` created can be closed
this way. An interactive LLDB that a person is using belongs to that person, so
closing it is refused.

## A Typical Session

Creating a session, debugging in it, and cleaning up looks like this:

```
session_create                            -> lldb-mcp://instance/4711/debugger/1
command "target create /tmp/hello"        -> Current executable set to '/tmp/hello' (arm64).
command "breakpoint set --name add"       -> Breakpoint 1: 4 locations.
command "run"                             -> Process 4713 stopped
                                             * thread #1, stop reason = breakpoint 1.1
                                                 frame #0: hello`add(a=2, b=3) at hello.c:2
command "frame variable"                  -> (int) a = 2
                                             (int) b = 3
command "continue"                        -> Process 4713 exited with status = 0
session_close                             -> deleted lldb-mcp://debugger/1
```

:::{note}
Sessions start in asynchronous mode, where `run` and `continue` return before
the process actually stops. Commands that need a stopped process then fail with
"Command requires a process which is currently stopped". Run
`script lldb.debugger.SetAsync(False)` once, right after `session_create`, to
get the synchronous behavior shown above.
:::

The debuggee's own output does not come back through MCP. Only debugger output
does. Redirect the program's output to a file and read it back if you need it.

## Attaching to a Running LLDB

Besides the sessions it creates, `lldb-mcp` can drive LLDB instances you are
already using, so an agent can inspect and steer the exact session you have in
front of you.

In that LLDB, start an MCP server:

```
(lldb) protocol-server start MCP
MCP server started with connection listeners: connection://[::1]:59999, connection://[127.0.0.1]:59999
```

The server picks a free port on localhost by default. To listen somewhere
specific, pass a URI, either `listen://[host]:port` for TCP or
`accept:///path/to/socket` for a Unix domain socket:

```
(lldb) protocol-server start MCP listen://localhost:59999
```

The server stops when LLDB exits, or explicitly:

```
(lldb) protocol-server stop MCP
```

`protocol-server get MCP` reports where a running server is listening. Starting
a server when one is already running, or stopping one that is not, is an error.

Once the server is up, that LLDB's sessions show up in `sessions_list` and
accept `command`, exactly like sessions `lldb-mcp` created. You do not need to
configure a port anywhere: each LLDB with a running MCP server records itself in
`~/.lldb`, and `lldb-mcp` finds it there.

:::{note}
Discovery happens once, when `lldb-mcp` starts. An LLDB you launch afterwards is
not picked up until the client reconnects to the MCP server, which usually means
restarting or reloading the MCP server in your client.
:::

## Resources

Resources are a primitive in the Model Context Protocol that allow servers to
expose content that can be read by clients. `lldb-mcp` exposes one resource per
debugger and one per target, across every session it can reach.

Debugger resources use the following URI:

```
lldb://instance/<pid>/debugger/<debugger id>
```

Example output:

```json
{
  "contents": [
    {
      "uri": "lldb://instance/4711/debugger/1",
      "mimeType": "application/json",
      "text": "{\"debugger_id\":1,\"name\":\"debugger_1\",\"num_targets\":1}"
    }
  ]
}
```

Debuggers can contain one or more targets, which are accessible using the
following URI:

```
lldb://instance/<pid>/debugger/<debugger id>/target/<target idx>
```

Example output:

```json
{
  "contents": [
    {
      "uri": "lldb://instance/4711/debugger/1/target/0",
      "mimeType": "application/json",
      "text": "{\"arch\":\"arm64-apple-macosx26.0.0\",\"debugger_id\":1,\"dummy\":false,\"path\":\"/tmp/hello\",\"platform\":\"host\",\"selected\":true,\"target_idx\":0}"
    }
  ]
}
```

Note that unlike the debugger id, which is unique, the target index is not
stable and may be reused when a target is removed and a new target is added.

## Troubleshooting

**"no debugger found" from `command`.** There is no session to run the command
in. Call `session_create` first, or pass the URI of an existing session.

**"Command requires a process which is currently stopped".** The session is in
asynchronous mode. Run `script lldb.debugger.SetAsync(False)` in it.

**"can only close sessions that lldb-mcp created".** `session_close` refuses to
tear down an interactive LLDB. Quit that LLDB yourself.

**A running LLDB does not show up in `sessions_list`.** Either its MCP server is
not running, which `protocol-server get MCP` will tell you, or it started after
`lldb-mcp` did. Restart the MCP server in your client to rediscover.

To see the JSON-RPC traffic between your client and `lldb-mcp`, set
`LLDB_MCP_LOG` in the environment. Messages are written to stderr, since stdout
carries the protocol.

The MCP server inside LLDB logs to the `Host` log channel:

```
(lldb) log enable lldb host
```

## Implementation

This section covers how the pieces fit together, for those working on LLDB
itself.

`lldb-mcp` is a multiplexer. It presents a single MCP server to the client and
fans out to one or more backends, each an LLDB MCP server reached over a socket
and identified by the pid of the process hosting it.

```
                           ┌──────────┐
                           │   LLDB   │
                           └────┬─────┘
                                │ socket
                                │
┌──────────┐              ┌─────┴─────┐              ┌──────────┐
│ in-proc  ├────socket────┤  lldb-mcp ├─────stdio────┤MCP Client│
│  LLDB    │              └─────┬─────┘              └──────────┘
└──────────┘                    │ socket
                                │
                           ┌────┴─────┐
                           │   LLDB   │
                           └──────────┘
```

There are two kinds of backend. The **local** backend is an MCP server that
`lldb-mcp` starts inside its own process, through `SBProtocolServer`. The
sessions it hosts are the ones `session_create` makes. **Remote** backends are
the separate LLDB processes discovered through the registry. Both are driven the
same way, over a socket through an `mcp::Client`, which keeps the tool
implementations in one place rather than special-casing the in-process path.

Requests are dispatched three ways. `initialize` and `tools/list` are answered
by the multiplexer directly. `sessions_list` and `resources/list` fan out to
every live backend and aggregate, keyed by pid so output is deterministic. A
backend that fails or has disconnected is omitted rather than failing the whole
listing. `command`, `resources/read`, and `session_close` are routed to a single
backend by the pid parsed out of the URI.

Backends only know their own local `lldb-mcp://debugger/{id}` and
`lldb://debugger/{id}` URIs. The multiplexer rewrites them into the
instance-qualified form in both directions, so a client never sees an ambiguous
id and a backend never sees a pid it does not understand.

`session_create` and `session_close` map onto the `debugger_create` and
`debugger_delete` tools on the local backend. Session ownership is enforced by
comparing the pid in the URI against the local backend's, which is why closing
someone else's session is refused. Running a command in one is not: any
`lldb-mcp` on the machine can drive any discovered session.

### Discovery

An LLDB that starts an MCP server writes `~/.lldb/lldb-mcp-<pid>.json`,
recording the pid and the URI to connect to. The entry is written only once the
server is listening, and removed on a clean exit. `lldb-mcp` reads the directory
at startup and connects to each entry, pruning any that fails to connect, since
that means the instance died without cleaning up. `lldb-mcp` registers itself
too, so its managed sessions are visible to other `lldb-mcp` processes.

### Inside `observe`

The tool lives in `lldb/source/Plugins/Protocol/MCP/` and is built from pieces
that can each be understood on their own:

| File | Responsibility |
|---|---|
| `ObservationPlan.{h,cpp}` | Parses and validates the plan; resolves each `at` to breakpoint locations and produces the did-you-mean for a name that matched nothing. |
| `ObservationEngine.{h,cpp}` | Launches the program, installs the tracepoints, records hits, and reports how the run ended. |
| `SerializeValue.{h,cpp}` | Renders a value tree to JSON under depth, node and cycle budgets. |
| `ValueNode.h`, `ValueObjectNode.{h,cpp}` | The interface the serializer works against, and the `ValueObject` adapter for it. The indirection is what makes the serializer testable without a process. |
| `Aggregate.{h,cpp}` | Accumulates the summary over every hit, and finds a repeating tail. |
| `EventArtifact.{h,cpp}` | Writes the JSONL event stream, under a bound. |
| `Tool.{h,cpp}` | The MCP tool: schema, description, and the call into the engine. |

Value resolution is shared with `dwim-print` through
`lldb/include/lldb/Target/DWIMValueResolution.h`, which decides between a
variable expression path and the expression evaluator and reports which one ran.
`dwim-print` keeps its dot-only rule for paths; `observe` opts into `->` and
`[]`, because a codebase of pointers would otherwise send every capture to the
expression evaluator.

Two shapes are deliberate. There is one tool rather than several, and no
`step`, `continue` or `run_to`: the tool list is a suggestion about how to work,
and offering those would advertise the loop this replaces. And the aggregate is
computed over **every** hit while the event stream is reduced — that invariant
is what makes a reduction a saving rather than a blind spot, and inverting it
would make `emit: on_change` lose exactly the rare value it is meant to surface.

#### LLDB behaviours worth knowing before changing this

Several plausible implementations compile and then silently do nothing. Each of
these was found the hard way.

**An ignore count does not work from a synchronous callback.**
`BreakpointLocation::IgnoreCountShouldStop` is reached only from
`StopInfoBreakpoint::PerformAction`, the asynchronous half of stop processing. A
synchronous callback that returns `false` makes `Thread::ShouldStop` bail before
`PerformAction` runs, so `Breakpoint::SetIgnoreCount` never takes effect.
`skip_first` is therefore counted in the callback.

**A breakpoint condition does not either**, for the same reason: conditions are
evaluated in `PerformAction`. `when` is evaluated by the callback itself.

**A one-shot breakpoint does not retire.** `PerformAction` removes it only when
the callback asks to stop, which a tracepoint never does. The engine reuses one
breakpoint per distinct return address and arms it with a counter, so the set is
bounded by call sites rather than by hits.

**Evaluating an expression from a synchronous callback costs a thread.**
`Breakpoint::SetCallback`'s own documentation warns against it. It does work —
`Process::RunThreadPlan` notices it is on the private state thread and spins up
a temporary one — but at roughly 20ms per evaluation, measured. That cost is the
whole reason for the adaptive control that disables an expensive capture partway
through a run.

**`Target::Launch` has no timeout.** It waits for the first stop with
`WaitForProcessToStop(std::nullopt, ...)` unconditionally, so `timeout_seconds`
covers the run but not the launch, and a debuggee that cannot start hangs the
tool. Bounding it needs `Target::Launch` to accept a timeout.

**`json::Value` built from a `StringRef` stores it by reference.** Passing a
temporary — `formatv(...).str()` is the usual way — leaves the value pointing at
freed memory. Build from `std::string`.

**`DenseMapInfo<uint64_t>` reserves `~0ULL`**, which is `LLDB_INVALID_ADDRESS`.
A `DenseSet<uint64_t>` of addresses asserts the first time it is handed one.

**A load address is not an identity.** A struct, its first member, and that
member's first member all begin at the same address, so address-only cycle
detection reports a nested aggregate as a cycle and loses the value it stood
for. `ValueObjectNode` combines the address with the type.

**`eDILModeSimple` parses only `.`**, so widening the set of accepted paths
means choosing the DIL mode to match, or every pointer path fails under
`target.experimental.use-DIL`.

#### Tests

`lldb/unittests/Protocol/` holds the unit tests: the plan parser, the
serializer, the aggregate, the artifact, and the engine's pure decisions —
emission, expression-cost control, and frame ranking. Everything needing a live
process is in `lldb/test/API/tools/lldb-mcp/observe/`, which drives the tool
over a Unix socket the way a real client does. That test needs to bind a socket,
so a sandbox that forbids it will fail the whole file at
`protocol-server start`.

### Adding Tools and Resources

The tool and resource-provider set lives in
`lldb/source/Plugins/Protocol/MCP/` and is installed by
`lldb_private::mcp::PopulateServer`. Sharing one installer keeps every MCP
server consistent, whether it runs in the plugin or is hosted in-process by an
embedder. Adding a tool means subclassing `lldb_protocol::mcp::Tool`, and adding
a resource means subclassing `lldb_protocol::mcp::ResourceProvider`, then
registering it there.

A tool added this way is exposed by the LLDB MCP server, not automatically by
`lldb-mcp`. Because the multiplexer owns the client-facing surface, it also
needs a case in `HandleToolsList` and `HandleToolsCall`, plus a routing decision
if its arguments carry a URI.

Note that the protocol version LLDB implements is `2024-11-05`, which has no
structured content. Tools return their output as text.
