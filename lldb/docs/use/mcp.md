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
expose functionality to clients. `lldb-mcp` exposes one.

A tool list is read as a suggestion about how to work, which is why there is
only one. A surface offering a session to create, a command to run in it and a
session to close describes the stepping loop `trace_program` exists to replace,
and it is the shape an agent already believes a debugger has, so it is the one
that gets reached for.

:::{note}
The MCP server inside LLDB — the one `protocol-server start MCP` starts, see
[Attaching to a Running LLDB](#attaching-to-a-running-lldb) — exposes more than
this: `command`, plus `debugger_list`, `debugger_create` and `debugger_delete`.
A client that connects straight to it rather than through `lldb-mcp` sees that
larger surface. This section describes `lldb-mcp`, which is what a client is
normally pointed at.
:::

### `trace_program`

Runs a program under a set of tracepoints and reports what happened, rather than
stepping through it. One call launches the program, reads the expressions named
at each tracepoint, lets the program run to its own end, and returns a summary
over every hit together with a JSONL file holding the full event stream.

It takes:

- `plan` (required): what to run and what to watch.
- `debugger` (optional, and normally omitted): the URI of an existing session to
  run in. With no session open and none named, `trace_program` opens one: a plan
  already carries the program, its arguments and its environment, so an empty
  session is not a decision a caller has to make first. Pass this only to run in
  a session you already have a URI for, such as an LLDB someone is using
  interactively. Session URIs look like this:

```
lldb-mcp://instance/{pid}/debugger/{id}
```

The `pid` identifies the process hosting the session and the `id` identifies the
debugger inside it. A session `trace_program` opened for itself lives until the
`lldb-mcp` process exits, which is when the client disconnects; nothing closes
one earlier, and a later call with no `debugger` reuses it.

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
        "capture": ["I.VTy->ID", "I.SubclassID"],
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

A line number means whatever the line table says, and the line table was written
when the binary was built — so a `file:line` tracepoint that outlives an edit
traces a different statement while still reporting `resolved_locations: 1`. Where
the source is newer than the binary its line table came from, the observation
carries `source_newer_than_binary` beside that count, naming the file and how far
apart the two are. It is the ordering of two mtimes and nothing more: not what
moved, not that anything did. Minutes usually means somebody edited between two
runs; days usually means a tree checked out after the build. Nothing is said when
either mtime cannot be read — a binary built on another machine names source
directories this one does not have — because a check that fires when it cannot
tell is a check that gets ignored.

An observation may also carry `when` (a condition), `called_from` (restrict to
hits reached from another function), `enabled_after` (hold it disabled until
another observation has been hit), `skip_first`, `only_hit`, `backtrace`,
`depth`, and `on` set to `entry` or `return`. `emit` is `every_hit`,
`on_change`, or `first_and_last`.

An `on: return` observation is taken where the function returns to, at which
point its frame has already been popped. The value it produced is captured as
`$return`, and state that does not live in the frame — a global, a static — still
reads. Its own parameters and locals do not, and are reported as unavailable
rather than being read from the caller's frame, which is the frame that is
current there: for a recursive function that substitution would return the
caller's value under the callee's name, shifted by exactly one frame and
indistinguishable in the aggregate from the right answer. Capture those at
`on: entry`.

The plan itself also takes `args`, `env`, `cwd`, `stdin` (a path whose contents
are fed to the program), `capture_inferior_output` (on unless turned off), and
`no_progress_seconds`, which ends a run that goes that long without any
tracepoint in the plan being hit. `timeout_seconds` is measured from the launch
rather than from the call, so reading a large binary's debug info -- reported
separately as `setup_ms` -- does not spend the budget for running it. That last one is off unless asked for,
because a plan whose triggers only fire near the end of a long run is
legitimate.

A run whose tracepoints all resolved and none of which ever fired says so in
`notes`, and names `no_progress_seconds` there. Each observation reports `hits: 0`
truthfully, and drawing the conclusion from a list of them is left to the reader
otherwise — which for a plan that spends its whole timeout getting there is a
whole timeout too late. No value is suggested: nothing in the run knows whether
those tracepoints were unreachable or merely late.

**An empty `observe` list is crash triage.** The program runs untouched and the
result is how it ended, with a ranked backtrace, locals and source at the
failure. That is the shortest useful plan:

```json
{"plan": {"program": "build/bin/opt", "args": ["-passes=instcombine", "repro.ll"]}}
```

For a program that hangs rather than crashes this is the first step and not the
last. Alongside the stack where the run was stopped it reports `profile`: where
stacks sampled during the run found the program, ranked, with the path it was
reached by. Every other part of a plan answers a question the caller already knew
to ask — a tracepoint has to name a function or a line — and this one does not, so
it is what to reach for when the question is which code is running rather than
whether a particular function is. A `cycle`, on the other hand, needs tracepoints
to emit the events it is found in, so run it again with an observation on each
function the profile or the backtrace named to get the repeating block.

#### Comparing two runs

`compare` holds the runs to make, each one the plan with a few fields replaced:

```json
{"plan": {"program": "build/bin/opt",
          "args": ["-passes=vector-combine", "-S", "in.ll"],
          "observe": [{"at": "foldShuffleToIdentity", "capture": ["I.VTy->ID"]}],
          "compare": [{"label": "fixed"},
                      {"label": "before", "program": "/tmp/opt.before"}]}}
```

The tracepoints are held still and what runs under them varies, because that is the
shape of every question worth asking twice: the same input under the binary before
and after a change, or the same binary over the input that fails and the one that
does not. A variant may replace `program`, `args`, `env`, `cwd` and `stdin`, and
nothing else — varying the observations would produce reports with nothing to line
up, and varying the timeout would make the runs incomparable in the dimension a
comparison is most often about. Runs are made one at a time, since one debugger
drives one process, and at most four in a call: each is a launch and a debug-info
read, so a longer series is a series of calls whose answers are read one at a time.

The response is the differences rather than one report per run. `runs` is a row per
run — how it ended, how long it took, its hit counts — and then:

- `diverged` names each thing that differed and what each run said about it. What is
  compared is how each run ended, its exit status and the line it stopped on, what it
  wrote on `stdout` and on `stderr`, and per observation its counts and each
  capture's value under `<label>.<expr>`. How a capture resolved is compared apart
  from its value, at `<label>.<expr>.capture`, and only where it resolved some way
  other than as a plain variable path. What a capture cost is not compared at all:
  two timings are never equal, and a clock is a property of the machine rather than
  of the run.
- `first_divergent_hit` is the hit at which the runs stopped agreeing, with what
  each saw there keyed by capture. For a miscompile that is the answer: everything
  before it is the same computation and everything after is a consequence. The
  baseline is the first run that produced a result and every later run is compared
  against it, so `saw` carries the baseline plus each run that differed at that hit —
  a run that agreed there is not listed. When one run simply got further, that is not
  a disagreement about any hit and it says `identical_through` instead. The
  comparison is over the hits each run kept, and says when there were more than that.
- `capture_failures` and `notes` are reported once each, deduplicated across the
  runs, carrying an `in` that names the runs only where the runs disagree about them:
  two runs failing at the same name is one fault in the request rather than a
  difference between them.
- `agreed` is a list of **names**, not values: the answer to "did my change affect
  anything else" is a line rather than a diffing exercise.

Two things are shortened rather than given whole, because a comparison holds two of
everything and a response is charged for its size on every later turn. A pair of
capture summaries is cut to the histogram entries whose counts differ, keeping
`distinct` and `values_elided`, so a difference of one value among twenty-eight costs
one value and not two histograms. And a program's output never goes in the document:
identical output collapses to one name under `agreed`, and output that differs is
reported as `first_differing_line` with that line from each run.

A run that could not be made at all is a row carrying its error, and the other runs
still answer — a change that stops the program from starting is the difference being
looked for, not a reason to fail the call. Agreement is decided against the runs that
produced a result, so one run failing to launch does not turn every row of the others
into a one-sided difference.

#### Sampled stacks

`profile` appears for a run sampled enough times to have a shape -- one sample is
where the program happened to be a fifth of a second in, which for a program that
fails early is inside its command-line parser -- and at least once inside the
program's own code, whether or not the plan had tracepoints — though a plan whose tracepoints are being hit constantly
leaves no moment at which the program is running freely, so in practice it is a
free-running run that gets one.

`hot` ranks the places the program was found, keyed on the innermost frame of each
sample, which is self time rather than time on the stack. Frames that resolved to
source come first: a thread parked in a wait is sampled as often as one burning a
core and its innermost frame is the same every time, so counting alone reports the
idle thread as the hottest place in the program. Where no place holds a share of the
run, one entry stands for the list — in an unoptimized build self time scatters over
inlined accessors, and which of them was innermost is a fact about a getter rather
than about the program.

`under` is where the run was: the functions on the stack for at least half the
busiest thread's samples, innermost first. That is the field to read for a program
that is stuck. A function can be there without ever being a leaf, which is the usual
case — measured on a compiler looping inside one analysis, the function responsible
appeared in every sample and was the innermost frame of none. A recursive function
counts once per sample rather than once per frame. `tid` and `threads` appear only
for a program that had more than one thread to tell apart.

Sampling means stopping the program, which costs a round trip to the debug stub
each way — about 90 ms, measured. Left unbounded that inflates the wall clock the
run is judged against, and a program that needed most of its ceiling would be
reported as having hung because it was being profiled. The engine spends at most a
tenth of a run on it and stops sampling when that is used up, so a long run is
sampled tens of times and a short one barely at all.

#### Reading the result

The response carries `outcome` — `exited`, `crashed`, `timed_out` or
`no_progress` — a `plan_report` per observation, an `aggregate`, a `terminal`
event, a `profile` where one was sampled, and a pointer to the artifact.
`elapsed_ms` covers the whole call, and
`setup_ms` appears beside it when most of that went on creating the target,
reading its debug info and resolving the tracepoints. That cost is charged once
per binary image rather than per hit — measured at 9.1 s for the first run against
a 238 MB debug build of a compiler and 0.23 s for the next identical one — so it
says nothing about what observing another run costs, which is why it is reported
apart from the total rather than left inside it. A run that got stuck also carries a
`cycle`, the block of locations the end of the run kept traversing, which for a
program that did not terminate is usually the answer. The cycle is found in the
event stream rather than on the stack, so it appears only for a plan that had
tracepoints to emit events: a stuck run with an empty `observe` list reports
where it was stopped and no cycle. It also has to be a traversal rather than a
place — at least two distinct locations — and the captured values have to recur
along with the locations, since a loop that is getting somewhere visits the same
places holding different values at every iteration. Where a plan carries no
captures, the locations decide it alone. `inferior_output` holds
the program's own output, with `stdout` and `stderr` apart, and `notes` holds what
went wrong that no single observation owns.

Read `aggregate` first. For each captured expression it gives the distinct
values with counts, the changes between them, and `outliers`: the values seen
only once or twice among many hits. That last field is usually the answer.

`aggregate_covers: "partial"` appears beside it when a ceiling ended the run while
the program still had work to do, and every field under `aggregate` then describes
the hits that were reached rather than the program. That distinction is invisible
otherwise: a value the run never got to is missing from `values` and missing from
`outliers`, where missing already reads as "never happened". The accompanying note
gives the hit rate, which is what a second run's `timeout_seconds` should be sized
against.

A scalar capture is keyed by its value, so `values` is an object and a capture
that never varied collapses to `"false x4012"`. A capture with structure keeps
that structure: `values` becomes a list of `value`/`count` pairs and each `value`
is the document itself, in `transitions` and `outliers` as well. A document
cannot be an object key without the serialization escaping every quote in it,
which is how a captured `StringRef` used to come back as a line of backslashes
for the caller to undo by hand.

A value that could not be read is not in `aggregate` at all. It is an error about
the capture rather than a value the program took: counted among the values it
competes with the real ones for a bounded histogram, it reports a change the
program never made, and it turns up as the rare value a caller is told to read
first. It is reported once instead, in `capture_failures`, and the capture's own
`errors` count says how many hits it covers.

A change is reported as a `from`/`to` pair with the number of times the run made
it and the sequence of the first time. Counting the pairs rather than listing every
change is what makes a value that cycles readable: two values alternating come back
as two entries carrying half the hits each, however long the run is.

`outliers` needs two things to mean anything, and both are withheld rather than
approximated. There have to be enough hits for "rare" to be a claim about the run,
and the values have to repeat: where a capture renders something new at every hit —
an address, a node id — every value is seen once, so every value is an outlier,
which is another way of saying that none is.
That second case *says* so, as `"outliers": "withheld: …"` — a string where the
array would be, so a client reading it as a list gets a type error rather than an
empty one. Absent `outliers` therefore means one thing only: nothing was rare. The
two used to be the same silence, and in one run they were the same silence one
label apart, where a capture with `distinct: 30266` over 30,266 hits and one where
nothing was rare read identically. The short-run case stays silent, since a run of
a dozen hits has all of its counts in front of the reader. Where `outliers` is
present, `outliers_of` gives the hits they were rare among — the denominator of
the claim, and not recoverable once `values_elided` says the histogram is a
selection.
One vector type among four thousand integers, with the hit where it first
appeared, is the bug — computed rather than left to be found. Re-run with
`only_hit` set to that `first_hit` to record that single hit in full detail;
cost does not matter at one hit, so captures and backtrace depth can be as
generous as you like. Captures are read at that hit alone, so the aggregate of
such a run covers it rather than the whole run — which is what the run that named
the hit already reported. A capture whose value is a call that prints, which is
how a compiler dumps a node, is worth having here for the same reason: it runs
once, and what it printed arrives in `printed` beside it.

An outlier carries two numbers because they count different things.
`first_hit` counts that observation's own hits and is what `only_hit` takes.
`first_seq` numbers the whole event stream: it matches a line in the artifact
for any hit whose event the emission mode kept. They are equal only when a plan
holds a single observation.

`values`, `transitions` and `outliers` are all bounded, and report
`values_elided`, `transitions_elided` and `outliers_elided` beside themselves
when they drop anything. `distinct` always counts every value, so a shortened
histogram cannot be mistaken for the real cardinality, and outliers are ranked
rarest-first before the bound applies, so what a bound drops is the least rare
of them. Values and changes are ranked by count, since the question a bounded list
of counted things answers is which of them dominate; where none does — every entry
kept within an eighth of the same count, with most of the population dropped
regardless — one entry stands for the rest and the count beside it says how many.
An eighth rather than exact equality, because exactness makes the rendering depend
on where the run happened to be cut: the same program printed `{"0":1031}` when it
ran to the end and eight entries of 313, 313, 312, 312, 312, 312, 312, 312 when a
ceiling stopped it mid-pass.

A value that could not be read comes back as `unavailable` with the kind, and a
`reason`: the one line of the debugger's own diagnostic that says what went wrong,
which is what decides whether to re-spell the capture, move it to another
location, or stop asking. A capture that ran and produced nothing — a call
returning void — reads `(void)` rather than as a failure, and what it printed is
in `printed` beside that value.

Where such a call printed something — `N->dump()`, which is how a compiler is
asked to describe a node — the printed text *is* the value, marked
`printed_as_value`. So the histogram counts dumps, a transition names the two it
moved between, an outlier is the dump seen twice in four thousand hits, and a
comparison of two runs compares what each printed. An expression run for its
effect has its effect as its answer, and for a node that answer is the only
legible rendering there is: reading the field gives `NodeType: 193`, and the
renderer that turns 193 into `ISD::XOR` can only be called.

Three things are worth knowing about that text as a value. Leading and trailing
whitespace is dropped and CRLF is folded to LF, so a dump does not key differently
from its own trimmed form or because its printer chose `outs()` over `errs()`; the
interior is kept, since a multi-line dump is a multi-line value. It is bounded to
the same 300 characters as any other capture's rendering, and a bounded key says
so in the key itself — `... (+607 more chars, whole text hashes a3e7d872)`, with
the hash there so that two dumps agreeing for 300 characters are still two values.
`printed` keeps the text raw, per stream and whole, and `printed_value_elided`
beside the value says to go there. And where one expression printed on *both*
streams there is no single text to be the value — nothing orders a write on the
one against a write on the other — so that hit keeps `(void)` and its `printed`.

A capture that was stopped keeps its numbers, and the reason it was stopped is
reported once per observation under `stopped`, against the list of captures it
covers. The usual reason is a fact about where the observation is taken rather
than about any one expression — at a return site none of the function's own names
can be read — and stating it per capture repeated one sentence once per name.

Where a capture was stopped for cost after reading real values — which is what a
printer at a hot tracepoint does, since a call costs about 75 ms against a path's
0.17 ms — the note also says how many hits its part of `aggregate` covers, against
how many the observation went on to have. Nothing exempts a printer from cost
control; what changes is that a histogram over 65 of 4000 hits says so where it is
read, rather than looking like the run's distribution.

A capture that could not be read *at all* is reported at top level in
`capture_failures`, not only as a count beside the expression. Each entry names
the observation, the capture as it was written, a `reason` — `no_such_name`,
`no_such_member`, `malformed` or `not_available_here` — the `detail` the compiler
gave, whether the capture was `disabled`, and `candidates`: names that were in
scope, drawn from the frame's own parameters and locals for a name that does not
resolve and from the type's fields for a member it does not have. A `when`
condition that failed appears the same way, marked `field: "when"`. The list is
bounded and reports `candidates_of` — how many names it was selected from — so a
short list is not read as everything there was.

The distinction that decides how soon a capture is given up on is whether its
failure can come out differently later. A member a type does not have, a name
nothing declares, an expression that does not parse: these fail identically at
every hit, so they are stopped at the first one rather than after three, and
retrying them would only buy compiles. A value optimised out here, or reached
through a pointer that is null just now, is a different question at the next hit
and is kept. A capture given up on is tried again if a module loads, because a
name in a library that has not been loaded yet is spelled correctly and does not
resolve — and a capture that failed early and resolved later is reported as
resolved, since what is read is the state after the run rather than the first
attempt.

Where the expression evaluator repairs a capture with a fix-it and the repair
works, the fixed spelling is adopted for the rest of the run — otherwise every
remaining hit pays a failed parse and then the retry — and reported as `fixed_as`
beside the capture. The `aggregate` and `plan_report` stay keyed on the spelling
that was *sent*, so a request can be correlated with its histogram; `fixed_as` is
what connects the two. A fix-it that was offered and still did not resolve is
reported with `fix_applied: false` and is not adopted: it is a suggestion the
compiler could not make work either.

Function names in a backtrace have their template arguments replaced with an
ellipsis. What those arguments distinguish is one instantiation from another, and
the file and line reported beside the name already do that; in template-heavy code
they are most of its length.

`terminal.locals` holds the frame's own variables within one node budget shared
across all of them, so what a local gets depends on what the others need. The
cheap and specific come first — scalars, then the body's own aggregates, then the
parameters, then `this`, then the compiler's range-for temporaries — because a
budget spent in declaration order goes to `this` and never reaches the
loop-carried value a hang is explained by. A local too large to render collapses
to its own value with a note of how much was left out; the locals the budget did
not reach are named together in one `_elided` entry, because the name is what goes
in the next plan's `capture`.

`plan_report` keeps four numbers apart on purpose: how many locations the name
resolved to, how many times the tracepoint was hit, how many of those hits had a
true condition, and how many events were emitted. A misspelled function name, a
condition that never held, and code that never ran all produce no events, and
these numbers are what tell them apart. A name that resolved to nothing comes
back with the nearest names that do resolve, found by asking for each spelling one
character away from it rather than by comparing against every name in the program:
searching that way has to walk the debug information of every compile unit, which
on a 238 MB debug build of a compiler took 332 seconds and spent the run's whole
wall clock answering a typo. The search is bounded in time and says so when it was
cut short, and every name it suggests is one that would have resolved; a name that resolved when its library
loaded partway through the run comes back resolved, since the count is read after
the run rather than before it.

An observation hit on more than one thread also reports `threads`. Hit order,
change detection and the aggregate all cover the observation rather than one
thread, so past one thread the sequence is an interleaving — the `tid` on each
event is what separates it again. An `on: return` observation reports
`returns_abandoned` when a frame left without returning, which is what an
exception or a longjmp does to one: those calls produce no event, and observing
the function on entry is what counts all of them.

Each capture's own row states how many hits it came back with a value at. A
capture that resolved as a path and never failed collapses to `"path x4012"`,
`$return` to `"abi x120"`, a capture the program recorded for itself to
`"in_process x4012"`, and anything with more to say gets an object whose
`evaluations` and `errors` give the same figure. The count is there because the
alternative was to infer it: the row named the tier and nothing else, so "read at
every hit" was the absence of an `errors` field — which is what a silently failing
capture also looks like. With hand-guessed paths into a compiler's internals that
is the likely failure, and one run hedged against it by capturing four fields
redundantly so they could corroborate each other. `not_evaluated` is still its own
word: a tracepoint that never fired is not a capture that could not be read.

Events themselves live in the artifact, one JSON object per line, and the
response reports its path and field names. The last few events are included
inline only when the program ended badly, which is when they are wanted.

#### What the program does for itself

Stopping a program costs about a millisecond, and a tracepoint that stops at every
hit is therefore bounded at a few hundred hits a second however little it does
there. That is the ceiling on what a plan can watch, and `fast` — on by default —
removes it by compiling the tracepoint's own work into the program: the observed
function is recompiled from its own source with the condition and the captures
injected into it, and its entry is redirected to the copy. A condition that does
not hold then costs two instructions, and a captured value is written into a ring
the debugger reads once per few thousand values rather than once per value.
Measured on twenty thousand hits: the stopping path managed 222 a second, and the
program recorded all twenty thousand in under a second.

Each observation's `eval` says which it got — `"in-process"`, or `"stopped: "` and
the reason. Every refusal falls back to stopping and none is silent, because a
refusal costs speed and nothing else and a caller comparing hit counts between
runs is comparing what each of them paid. `on: return`, a tracepoint that matched
more than one place, a function whose source is not on disk or is newer than the
binary, and anything the compiler rejects all fall back.

`"in-process"` is not by itself the claim that nothing stopped: an observation
whose condition is compiled in still stops at every hit the condition lets
through, unless its captures went into the program too. Which of them did is on
each capture, where the granularity belongs — `"in_process x4012"` means the value
was copied out where the program held it, and that the hit cost nothing at all.
Captures go in only when doing so removes the stop entirely, since while the
debugger is standing in the frame it reads every capture there anyway. So a
`backtrace`, an `only_hit` or a `$return` keeps the stop, and a capture the copy's
own debug info says is not a scalar of eight bytes or fewer sends the whole
observation back to stopping rather than being dropped from it: a hit reported with
one of the values the caller asked for silently absent is worse than a slow one.

A hit the program recorded rather than stopped for carries no `tid` and no `t_ms`
on its event — nothing watched it happen, and the moment its record was read is
shared by the thousands of hits read with it. Its values, its order and its counts
are what a stop would have reported. The run also says once that it recompiled,
because the program under test really is running unoptimized copies of those
functions: slower than what it was built as, and where the original relied on what
the optimizer did, not always the same code. `"fast": false` leaves the program
exactly as it was built, at a stop per hit, which is what somebody measuring the
program's own timing needs.

The facility is arm64-only and tested on Darwin. Everywhere else every observation
falls back, and says so.

#### Writing a good plan

What matters about a capture is whether it is a **path** or a **call**, not how
it is spelled. `I.VTy->ID` and `I->VTy->ID` are both paths: `->` and `[]` are
as much a part of a path as `.` is, and unlike `dwim-print`, which accepts only
`.`, a capture may use them. `I->getType()` is a call, and that is the
distinction to care about.

Prefer a path. A path is a debug-info lookup and a memory read; a call compiles
an expression and runs it inside the observed process. On a tracepoint hit
thousands of times the difference decides whether the run finishes, and a call
that proves too expensive is measured and switched off partway through so the run
stays inside its timeout — yielding partial data where the path would have
yielded all of it. The report says when this happened and names the cheaper
spelling.

A path names what the type **declares**, which for a C++ class is not what it
exposes. Debug info describes storage, so where `llvm::Value` offers
`getType()` the path beside it is `VTy`, and access control does not enter into
it: a private field is as readable as a public one. Reading a field name off an
accessor is how a capture most often fails — `getName()` does not imply a `Name`,
and `size()` does not imply a `Size`. Where a member name is a guess, capture the
accessor call as well and let the cheaper of the two be the one that resolves.

#### Data formatters

What a value *renders* as is a separate question from what path reaches it, and
it is decided by data formatters. With one, a captured `llvm::StringRef` comes
back as the string it holds; without, it comes back as the two fields it is made
of, with the text a level down inside a `const char *`. Formatters are not built
in — a project ships a script to import, and LLVM's own is
`llvm/utils/lldbDataFormatters.py`.

A session `trace_program` opens for itself sources `~/.lldbinit`, the same file an
interactive lldb reads, so a developer who already imports their project's
formatters there gets the same renderings from a plan as from their own prompt.
`~/.lldbinit-lldb-mcp` is read too, which is what lets an agent-driven session be
configured apart from an interactive one. A session passed in as `debugger` keeps
whatever formatters it already has.

A run that met no formatter at all — nothing it rendered had a summary, and
something was expanded into its members for want of one — says so in `notes`. The
two cases are otherwise indistinguishable in the response and want opposite
responses: read the members, or load the formatters.

A capture whose value renders large comes back as the value's own value alone --
for a pointer, its address -- with a marker saying how much was left out. Depth and
node budgets bound the walk without bounding the reading, and a capture is read at
every hit and appears three times in a summary: as a histogram key, on each side of
a transition, and in the artifact. Past that size the address is the identifying
part, and what the object holds is a question for a path that names the field
wanted. Children that are all unreadable for one reason -- every member of an
object reached through a null pointer -- are reported as that one reason.

Capture more expressions than seems necessary. A capture costs wall clock once
per run, not tokens per exchange, and the alternative to capturing something now
is running the whole program again to ask one more question. A guess that was
wrong is cheap: a capture that cannot resolve is stopped at its first hit and
reported in `capture_failures` with the names that were in scope, so
over-capturing costs one compile per bad guess rather than one per hit.

## A Typical Session

Triage first. An empty `observe` list runs the program untouched and reports how
it ended, so it is what to send before knowing where to look — no session to open
first, and nothing to name but the program:

```json
{"plan": {"program": "build/bin/opt",
          "args": ["-passes=instcombine", "-S", "repro.ll"]}}
```

For a crash that comes back as `outcome: crashed` with a ranked backtrace, the
locals and the source at the failure. For a program that never finishes it comes
back as `timed_out` with `profile`: where sampled stacks found it, ranked, which
answers which code is running without having been told where to look.

Then ask about the code the first call named. Say `profile` put the run under
`InstCombinerImpl::visitAdd`; a second call watches it and reads values at every
hit:

```json
{"plan": {"program": "build/bin/opt",
          "args": ["-passes=instcombine", "-S", "repro.ll"],
          "observe": [
            {"at": "InstCombinerImpl::visitAdd",
             "capture": ["I.VTy->ID", "I.SubclassID", "I.getName()"],
             "emit": "on_change"},
            {"at": "InstCombinerImpl::visitAdd", "on": "return",
             "capture": ["$return"]}]}}
```

That returns a `plan_report` per observation, an `aggregate` over every hit, and
the path to a JSONL artifact holding the event stream. Read `aggregate` first,
and its `outliers` first of all: one value seen twice among four thousand hits,
with the hit at which it first appeared, is usually the answer. Re-run with
`only_hit` set to that `first_hit` to record that one hit in full detail, where a
call is affordable because it runs once.

The debuggee's own output does not come back as debugger output. It is reported in
`inferior_output`, with `stdout` and `stderr` apart.

A capture that prints instead of returning a value — `I->dump()`, which is how an
object that knows how to describe itself is read — comes back with what it printed
in `printed` beside it, per hit and per stream, and with that text as the capture's
own value. It is therefore a first-class observable: `emit: on_change` over a
printer emits when what it prints changes rather than never, and the aggregate
summarises dumps rather than one bucket of `(void)`. See `printed_as_value` above
for what is normalised on the way.

The two streams are separated by giving the program's standard error a file of the
run's own. Standard output stays on a terminal, which keeps it line-buffered so
that a program that hangs is still reported with its last line — the reason a run
reports the program's output at all — and which is also why its newlines arrive as
CRLF. Standard error is unbuffered whatever it is connected to, so it loses nothing
by moving, and a file read at an offset is readable the instant an expression
returns rather than whenever LLDB's reader thread next runs.

Attribution is best-effort, and the two ways it is approximate are worth knowing.
The run flushes the program's streams around a capture that prints, so text the
program had written and not yet flushed when that capture ran is credited to the
capture. And a capture is only known to print once it has printed, so the first
hit at which one does may have its text reported as the program's own.

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

Once the server is up, that LLDB's sessions are reachable from `lldb-mcp`
exactly like the one it opens for itself: pass the session's URI as
`trace_program`'s `debugger` argument. You do not need to configure a port
anywhere: each LLDB with a running MCP server records itself in `~/.lldb`, and
`lldb-mcp` finds it there.

:::{warning}
**No tool enumerates sessions.** A session is reachable but not discoverable
through the tool surface, which offers `trace_program` and nothing else, so a
`debugger` URI has to come from somewhere else.

`resources/list` is that somewhere for a client that reads resources: it lists
every debugger `lldb-mcp` can reach, across every instance, as
`lldb://instance/<pid>/debugger/<id>`. The `debugger` argument for the same
session is that URI under the other scheme,
`lldb-mcp://instance/<pid>/debugger/<id>`. Failing that, the `pid` is the LLDB
process's own and the `id` of its first session is `1`.
:::

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

**"no debug session available".** `lldb-mcp` has no in-process backend to open a
session in, which means the local backend failed to start. The stderr log below
says why.

**"no debugger found" or "malformed debugger specifier".** The `debugger`
argument names a session that is not there, or is not written as
`lldb-mcp://instance/<pid>/debugger/<id>`. Nothing enumerates sessions any more —
see [Attaching to a Running LLDB](#attaching-to-a-running-lldb) for where a URI
can come from. Omitting `debugger` avoids the question: `trace_program` opens a
session when there is none.

**A running LLDB is not reachable.** Either its MCP server is not running, which
`protocol-server get MCP` will tell you, or it started after `lldb-mcp` did.
Restart the MCP server in your client to rediscover.

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
sessions it hosts are the ones `trace_program` opens. **Remote** backends are
the separate LLDB processes discovered through the registry. Both are driven the
same way, over a socket through an `mcp::Client`, which keeps the tool
implementations in one place rather than special-casing the in-process path.

Requests are dispatched three ways. `initialize` and `tools/list` are answered
by the multiplexer directly. `resources/list` fans out to every live backend and
aggregates, keyed by pid so output is deterministic; a backend that fails or has
disconnected is omitted rather than failing the whole listing. `tools/call` and
`resources/read` are routed to a single backend by the pid parsed out of the URI,
and a `tools/call` naming anything but `trace_program` is refused rather than
forwarded — a tool a backend implements does not reach a client until the
multiplexer decides to advertise it.

Backends only know their own local `lldb-mcp://debugger/{id}` and
`lldb://debugger/{id}` URIs. The multiplexer rewrites them into the
instance-qualified form in both directions, so a client never sees an ambiguous
id and a backend never sees a pid it does not understand.

A `trace_program` call that names no `debugger` is forwarded to the local backend
with that argument stripped, leaving the backend to pick a session or open one.
Nothing in the multiplexer creates or destroys a session: the process exiting is
what releases them, which happens when the client disconnects. Running a
`trace_program` in someone else's session is not restricted — any `lldb-mcp` on
the machine can drive any discovered session.

### Discovery

An LLDB that starts an MCP server writes `~/.lldb/lldb-mcp-<pid>.json`,
recording the pid and the URI to connect to. The entry is written only once the
server is listening, and removed on a clean exit. `lldb-mcp` reads the directory
at startup and connects to each entry, pruning any that fails to connect, since
that means the instance died without cleaning up. `lldb-mcp` registers itself
too, so its managed sessions are visible to other `lldb-mcp` processes.

### Inside `trace_program`

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
`dwim-print` keeps its dot-only rule for paths; `trace_program` opts into `->` and
`[]` (as [Writing a good plan](#writing-a-good-plan) says), because a codebase of
pointers would otherwise send every capture to the expression evaluator.

Two shapes are deliberate. There is one tool rather than several — no `step`,
`continue` or `run_to`, and no session to create, run commands in and close: the
tool list is a suggestion about how to work, and offering those would advertise
the loop this replaces. That is why the multiplexer's `tools/list` is one entry
and its `tools/call` refuses every other name. And the aggregate is
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
breakpoint per distinct return address, so the set is bounded by call sites
rather than by hits.

**A return address is not a frame.** Because that breakpoint is shared, what
says whether a stop there is the return of the frame that armed it has to be
kept separately — `ArmedFrames`. Two threads reach the same return address, so
counting arrivals reports one thread's return as another's. A frame unwound by
an exception or a longjmp never reaches it at all, so counting arrivals also
waits forever for that frame and then credits the next call's return to it. A
frame is identified by its thread and its call-frame address, and the stack
grows down, so a frame is known to be gone once its thread is seen at or below
it — which costs no unwinding, since the current frame's own CFA is enough. The
same test is what makes `called_from` mean "a frame of that function is still
below this one, on this thread" rather than "the gate was entered at some point
by somebody".

**Evaluating an expression from a synchronous callback costs a thread.**
`Breakpoint::SetCallback`'s own documentation warns against it. It does work —
`Process::RunThreadPlan` notices it is on the private state thread and spins up
a temporary one — but at roughly 20ms per evaluation, measured. That cost is the
whole reason for the adaptive control that disables an expensive capture partway
through a run.

**`StackID` carries two call-frame addresses.** The metadata-bearing one can hold
pointer-authentication bits, which do not survive being compared as numbers.
Ordering frames on a stack needs `GetCallFrameAddressWithoutMetadata`.

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
emission, expression-cost control, capture-failure classification and candidate
ranking, frame arming, and frame ranking. Everything
needing a live process is in `lldb/test/API/tools/lldb-mcp/observe/`, which
drives the tool over a Unix socket the way a real client does. That test needs to
bind a socket, so a sandbox that forbids it will fail the whole file at
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
if its arguments carry a URI. That the two surfaces differ is deliberate rather
than an oversight: `command` and the `debugger_*` tools are registered here and
are reachable by a client that connects to an LLDB's own MCP server, and
`lldb-mcp` advertises `trace_program` alone.

Note that the protocol version LLDB implements is `2024-11-05`, which has no
structured content. Tools return their output as text.
