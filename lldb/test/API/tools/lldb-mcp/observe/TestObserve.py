"""
Test the MCP `observe` tool end to end: a program run under an observation plan,
reported as one JSON document plus a JSONL event artifact.
"""

import json
import os
import shutil
import socket
import tempfile

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *

# Unix domain socket paths are limited to around 104 characters on macOS and 108
# on Linux, and the limit covers the whole path rather than the file name.
MAX_SOCKET_PATH_LENGTH = 104

# The version lldb's MCP server speaks, from lldb/include/lldb/Protocol/MCP.
PROTOCOL_VERSION = "2024-11-05"

# How long to wait for one MCP reply. A plan carries its own wall-clock ceiling,
# so this only has to outlast that plus the launch, and it exists to turn a
# wedged server into a failure rather than a hang.
REPLY_TIMEOUT_SECONDS = 900


def serialized_scalar(rendered):
    """The scalar behind an aggregate's histogram key.

    A scalar capture is keyed by the value itself, so the integer 7 arrives as
    "7" rather than as the document it serializes to. A capture with structure
    keeps the document, because there the structure is the information.
    """
    if rendered.startswith("{"):
        value = json.loads(rendered)
        if isinstance(value, dict) and "value" in value:
            raise AssertionError(
                f"a scalar should be keyed by its value, not by {rendered}"
            )
        raise AssertionError(f"expected a scalar key, got {rendered}")
    return rendered


def documented_example(description):
    """The worked plan out of a tool description, as an object.

    Brace-matched from the first `{"plan"` rather than found by line or by a
    fixed slice, so rewrapping the prose around the example changes nothing
    here. The example carries no brace inside a string, and one added later
    fails this rather than being matched wrongly.
    """
    start = description.find('{"plan"')
    if start < 0:
        raise AssertionError(f"no worked example in {description!r}")
    depth = 0
    for end in range(start, len(description)):
        if description[end] == "{":
            depth += 1
        elif description[end] == "}":
            depth -= 1
            if depth == 0:
                return json.loads(description[start : end + 1])
    raise AssertionError("the example's braces are unbalanced")


def capture_tier(capture):
    """The tier out of a rendered capture report.

    A capture that resolved as a path and never failed collapses to the tier plus
    the count of hits it read at, `"path x12"`, since that is all it has to say.
    """
    if isinstance(capture, str):
        return capture.split(" x")[0]
    return capture["tier"]


def capture_reads(capture):
    """How many hits a capture came back with a value at.

    Stated on the row so that "read at every hit" is a positive claim rather than
    the absence of an `errors` field, which is what a silently failing capture
    looks like too.
    """
    if isinstance(capture, str):
        return int(capture.split(" x")[1]) if " x" in capture else 0
    return capture["evaluations"] - capture.get("errors", 0)


class MCPConnection:
    """A client for lldb's MCP server: JSON-RPC, one message per line."""

    def __init__(self, path):
        self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._socket.settimeout(REPLY_TIMEOUT_SECONDS)
        self._socket.connect(path)
        # A reply can be tens of kilobytes and arrive in several reads, so the
        # framing is done by a buffered reader rather than by hand.
        self._reader = self._socket.makefile("rb")
        self._last_id = 0

    def close(self):
        self._reader.close()
        self._socket.close()

    def _send(self, message):
        self._socket.sendall((json.dumps(message) + "\n").encode("utf-8"))

    def notify(self, method, params=None):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        self._send(message)

    def request(self, method, params=None):
        self._last_id += 1
        request_id = self._last_id
        message = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            message["params"] = params
        self._send(message)

        while True:
            line = self._reader.readline()
            if not line:
                raise AssertionError(
                    f"the MCP server closed the connection during {method}"
                )
            reply = json.loads(line)
            # Anything carrying another id, or none, is a notification or a
            # reply to an earlier request; only the matching one ends the wait.
            if reply.get("id") == request_id:
                return reply

    def initialize(self):
        self.request(
            "initialize",
            {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": {"name": "TestObserve", "version": "1.0"},
            },
        )
        self.notify("notifications/initialized")


@skipIfRemote
@requirePOSIX
class ObserveTestCase(TestBase):
    NO_DEBUG_INFO_TESTCASE = True

    def setUp(self):
        TestBase.setUp(self)
        self._connection = None

    def connect(self):
        """Starts an MCP server inside this lldb and connects to it.

        `observe` is reachable only over MCP -- no command exposes it -- so the
        test speaks the protocol to the server the `protocol-server` command
        starts, which is the same path a real client takes.
        """
        if self._connection is not None:
            return self._connection

        directory = tempfile.TemporaryDirectory()
        socket_file = os.path.join(directory.name, "mcp.sock")
        if len(socket_file) >= MAX_SOCKET_PATH_LENGTH:
            directory.cleanup()
            self.skipTest(
                f"Socket path {socket_file} exceeds the "
                f"{MAX_SOCKET_PATH_LENGTH} character limit"
            )

        self.runCmd(f"protocol-server start MCP accept://{socket_file}")
        self.addTearDownHook(
            lambda: self.runCmd("protocol-server stop MCP", check=False)
        )

        self._connection = MCPConnection(socket_file)
        self.addTearDownHook(self._connection.close)
        # Hooks run in the order they are added, so the directory holding the
        # socket goes last: the server is still listening on it until then.
        self.addTearDownHook(directory.cleanup)

        self._connection.initialize()
        return self._connection

    def observe(self, plan):
        """Runs one observation plan and returns the document `observe` reported."""
        reply = self.connect().request(
            "tools/call",
            {
                "name": "trace_program",
                # The debugger is named rather than defaulted, so the plan runs
                # in this test's own debugger whatever else exists.
                "arguments": {"debugger": str(self.dbg.GetID()), "plan": plan},
            },
        )

        # A run that crashed, hung or observed nothing is reported in the
        # document; a protocol-level error means the call never got that far.
        self.assertNotIn("error", reply, str(reply.get("error")))
        content = reply["result"]["content"]
        self.assertFalse(reply["result"].get("isError", False), str(content))
        # The whole report is one JSON document inside a single text block.
        self.assertEqual(len(content), 1, str(content))
        document = json.loads(content[0]["text"])

        # The engine writes each run's event stream into its own unique
        # directory. Removing it is deferred, because a test reads the file
        # after the reply arrives.
        path = document.get("artifact", {}).get("path")
        if path:
            self.addTearDownHook(
                lambda: shutil.rmtree(os.path.dirname(path), ignore_errors=True)
            )
        return document

    def tool_description(self, name):
        """What the server advertises for one tool.

        Read over the protocol rather than out of the header, because the string
        a caller acts on is the one that arrives in tools/list.
        """
        reply = self.connect().request("tools/list")
        for tool in reply["result"]["tools"]:
            if tool["name"] == name:
                return tool["description"]
        raise AssertionError(f"no tool named {name}: {reply}")

    def events(self, document):
        """The artifact's events, one per line, as objects."""
        path = document["artifact"]["path"]
        with open(path, "r") as stream:
            return [json.loads(line) for line in stream.read().splitlines() if line]

    def test_a_run_releases_everything_it_allocated(self):
        """A finished run leaves the debugger as it found it.

        The engine runs in a debugger it does not own, so a target it fails to
        release outlives the call and takes every module the program loaded with
        it -- around eighty for a trivial binary here, and again for every
        subsequent call. Asserted directly rather than left to the suite's
        teardown check, because that check reports a number without saying which
        of the things a run allocates was kept.
        """
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [{"at": "classify_token", "capture": ["depth"]}],
            }
        )
        self.assertEqual(document["outcome"], "exited", str(document))

        # The target the run made is gone from the list it was added to.
        self.assertEqual(self.dbg.GetNumTargets(), 0)

        # And released, not merely unlisted. A target still holding a reference
        # to itself keeps its modules out of reach of the collector, so the count
        # after collecting is what distinguishes the two.
        lldb.SBModule.GarbageCollectAllocatedModules()
        self.assertEqual(lldb.SBModule.GetNumberAllocatedModules(), 0)

    def test_empty_plan_is_crash_triage(self):
        """A plan with no observations reports how the program died."""
        self.build()
        crash_line = line_number("main.c", "the null dereference")

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "args": ["crash"],
                "timeout_seconds": 120,
            }
        )

        self.assertEqual(document["outcome"], "crashed", str(document))
        # No observations means no plan report at all, which is what makes the
        # degenerate plan crash triage rather than a tracing run that saw
        # nothing.
        self.assertNotIn("plan_report", document)

        terminal = document["terminal"]
        self.assertNotEqual(terminal["description"], "")
        # An exit status belongs to a program that reached its own end.
        self.assertNotIn("exit_status", terminal)
        self.assertEqual(terminal["function"], "crash_now")
        self.assertIn("main.c", terminal["file"])
        self.assertEqual(terminal["line"], crash_line)

        functions = [frame["function"] for frame in terminal["frames"]]
        self.assertIn("crash_now", functions, str(terminal["frames"]))
        self.assertIn("main", functions, str(terminal["frames"]))
        # The ranked list is a reduction of what the unwinder produced.
        self.assertGreaterEqual(terminal["frames_total"], len(terminal["frames"]))

        self.assertIn("null_pointer", terminal["locals"], str(terminal["locals"]))
        self.assertIn("crash_now", terminal["source"])

    def test_the_documented_example_runs(self):
        """The worked example in the tool's own description resolves every
        capture it names.

        This is the one string a caller reads before it has read anything else,
        and it is copied for its shape, so a capture spelled in a way that does
        not resolve is a failure handed to everyone who copies it. Two fields are
        replaced -- the program, which is this suite's binary rather than the
        description's plausible one, and a ceiling generous enough for a slow
        bot. Everything else runs as the description writes it, so drift on
        either side lands here.
        """
        self.build()

        example = documented_example(self.tool_description("trace_program"))
        plan = example["plan"]
        plan["program"] = self.getBuildArtifact("a.out")
        plan["timeout_seconds"] = 300

        document = self.observe(plan)
        self.assertEqual(document["outcome"], "exited", str(document))

        # The claim the example makes by being an example: nothing in it needs
        # repairing, and nothing in it names something that is not there.
        self.assertEqual(document.get("capture_failures", []), [], str(document))

        entry = document["plan_report"]["classify_token"]
        self.assertEqual(entry["hits"], 3, str(entry))
        for expression in ["tok->kind", "tok->text", "depth"]:
            capture = entry["captures"][expression]
            # A capture that resolved as a path and never failed collapses to the
            # tier alone, so this asserts both that it worked and that reaching
            # through the pointer did not cost an expression evaluation.
            self.assertEqual(capture_tier(capture), "path", str(capture))
            # And the row says how many hits it read at, rather than leaving that
            # to be inferred from the observation's `hits` and the absence of an
            # `errors` field.
            self.assertEqual(capture_reads(capture), 3, str(capture))

        kinds = document["aggregate"]["classify_token"]["tok->kind"]["values"]
        self.assertEqual(
            {serialized_scalar(key): count for key, count in kinds.items()},
            {"1": 1, "2": 1, "4": 1},
        )

        # The second tracepoint: a return value in on_change mode, over four
        # calls that each return something different.
        returns = document["plan_report"]["parse_expr"]
        self.assertEqual(returns["hits"], 4, str(returns))
        self.assertEqual(returns["emitted"], 4, str(returns))
        self.assertEqual(
            document["aggregate"]["parse_expr"]["$return"]["distinct"], 4, str(returns)
        )

    def test_on_change_collapses_a_loop(self):
        """on_change emits once per run of identical hits, and the aggregate
        still counts every hit."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 120,
                "observe": [
                    {
                        "at": "record_bucket",
                        "capture": ["bucket"],
                        "emit": "on_change",
                    }
                ],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))

        report = document["plan_report"]["record_bucket"]
        self.assertGreaterEqual(report["resolved_locations"], 1)
        self.assertEqual(report["hits"], 100)
        # main.c passes i / 25, so the parameter takes four values over the
        # hundred calls and on_change emits at each change.
        self.assertEqual(report["emitted"], 4)
        # The condition counters are reported only for an observation that
        # carried a condition, so their absence says "no condition" rather than
        # "a condition that never held".
        self.assertNotIn("condition_true", report)
        self.assertNotIn("condition_errors", report)
        # A parameter is a debug-info lookup and a memory read, not an
        # expression compiled and run in the observed process.
        self.assertEqual(capture_tier(report["captures"]["bucket"]), "path")

        aggregate = document["aggregate"]["record_bucket"]["bucket"]
        self.assertEqual(aggregate["distinct"], 4)
        counts = {
            serialized_scalar(rendered): count
            for rendered, count in aggregate["values"].items()
        }
        self.assertEqual(counts, {"0": 25, "1": 25, "2": 25, "3": 25})

        # Aggregation runs over hits rather than over emitted events, so the
        # sequence a transition carries is numbered by hit. Each pair is counted
        # rather than listed, so a value that cycles does not render one entry
        # per change.
        transitions = [
            (
                transition["first_seq"],
                serialized_scalar(transition["from"]),
                serialized_scalar(transition["to"]),
                transition["count"],
            )
            for transition in aggregate["transitions"]
        ]
        self.assertEqual(
            transitions,
            [(26, "0", "1", 1), (51, "1", "2", 1), (76, "2", "3", 1)],
        )

        # Twenty-five of each is not rare.
        self.assertNotIn("outliers", aggregate)

        self.assertEqual(document["artifact"]["events"], 4)

    def test_misspelled_function_fails_loudly(self):
        """A name that matched no code comes back as zero resolved locations and
        a suggestion, not as an observation that silently never fired."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 120,
                "observe": [{"at": "compute_valu"}],
            }
        )

        # The program still ran; only the observation could not fire.
        self.assertEqual(document["outcome"], "exited", str(document))

        report = document["plan_report"]["compute_valu"]
        self.assertEqual(report["resolved_locations"], 0)
        self.assertEqual(report["hits"], 0)
        error = report["error"]
        self.assertIn('no code matched "compute_valu"', error)
        self.assertIn("Did you mean", error)
        self.assertIn("compute_value", error)

    def test_counters_stay_distinct(self):
        """hits, condition_true and condition_errors answer three different
        questions: whether the code ran, whether the condition ever held, and
        whether the condition was even valid."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                # A hundred conditions each compile and evaluate, which makes
                # this the one plan here whose per-hit cost is not a memory
                # read.
                "timeout_seconds": 600,
                "observe": [
                    {
                        "at": "record_value",
                        "capture": ["value"],
                        "when": "value == 4242",
                    }
                ],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))

        report = document["plan_report"]["record_value"]
        # The code ran a hundred times...
        self.assertEqual(report["hits"], 100)
        # ...the condition never held...
        self.assertEqual(report["condition_true"], 0)
        # ...and it was a valid condition at every one of them.
        self.assertEqual(report["condition_errors"], 0)
        self.assertEqual(report["emitted"], 0)

        # A hit whose condition was false is not captured either, so the capture
        # reports having not been evaluated rather than a hundred failures -- and
        # says it as "not_evaluated", not with the word a capture that was read and
        # failed gets. A word, because the numbers beside it would all be zero.
        capture = report["captures"]["value"]
        self.assertEqual(capture, "not_evaluated")

        # Nothing was recorded, so there is nothing to aggregate.
        self.assertNotIn("aggregate", document)

    def test_artifact_is_parseable_jsonl(self):
        """The event stream is a file of one JSON object per emitted event."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 120,
                "observe": [{"at": "record_value", "capture": ["value"]}],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        self.assertEqual(document["plan_report"]["record_value"]["emitted"], 100)

        artifact = document["artifact"]
        # The last few events are inlined only when the program ended badly.
        self.assertNotIn("tail", artifact)
        self.assertNotIn("truncated", artifact)
        self.assertEqual(artifact["events"], 100)
        self.assertTrue(os.path.exists(artifact["path"]), artifact["path"])

        with open(artifact["path"], "r") as stream:
            lines = [line for line in stream.read().splitlines() if line]
        self.assertEqual(len(lines), artifact["events"])

        events = [json.loads(line) for line in lines]

        # The response says how to read the file; every line must actually carry
        # what it advertises, or a reader greps for a field that is not there.
        self.assertEqual(artifact["format"], "one JSON object per line")
        for name in artifact["fields"]:
            self.assertIn(name, events[0], f"{name} advertised but not written")

        # One observation numbers its hits and the whole event stream
        # identically, since the sequence counts every recorded hit.
        self.assertEqual([event["seq"] for event in events], list(range(1, 101)))
        for event in events:
            self.assertEqual(event["label"], "record_value")
            self.assertEqual(event["frame"], "record_value")
            self.assertIsInstance(event["t_ms"], (int, float))
            self.assertIn("value", event["values"], str(event))

        # Hits of one observation are numbered and compared as a single
        # sequence, so the thread is what lets a reader take that sequence apart
        # again. One thread here, and every event says which.
        threads = {event["tid"] for event in events}
        self.assertEqual(len(threads), 1, str(threads))
        self.assertGreater(threads.pop(), 0)

    def test_outlier_is_found(self):
        """A value seen once among a hundred hits is named, with the sequence
        number of the hit that showed it."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 120,
                "observe": [
                    {
                        "at": "record_value",
                        "capture": ["value"],
                        "emit": "on_change",
                    }
                ],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))

        report = document["plan_report"]["record_value"]
        self.assertEqual(report["hits"], 100)
        # 7, then 99 at the forty-third call, then 7 again.
        self.assertEqual(report["emitted"], 3)

        aggregate = document["aggregate"]["record_value"]["value"]
        self.assertEqual(aggregate["distinct"], 2)
        counts = {
            serialized_scalar(rendered): count
            for rendered, count in aggregate["values"].items()
        }
        # Ninety-seven of these hundred hits were never emitted, and the
        # aggregate counted all of them anyway.
        self.assertEqual(counts, {"7": 99, "99": 1})

        outliers = aggregate["outliers"]
        self.assertEqual(len(outliers), 1, str(outliers))
        self.assertEqual(serialized_scalar(outliers[0]["value"]), "99")
        self.assertEqual(outliers[0]["count"], 1)
        # main.c passes 99 at i == 42, which is the forty-third call.
        self.assertEqual(outliers[0]["first_seq"], 43)
        # An outlier carries two numbers because they count different things.
        # With one observation they agree; test_outlier_numbers_are_distinct
        # covers the case where they do not.
        self.assertEqual(outliers[0]["first_hit"], 43)

    def test_outlier_numbers_are_distinct(self):
        """first_hit counts one observation's hits, first_seq the whole stream."""
        self.build()

        # record_bucket runs a hundred times before record_value is reached, so
        # the two numbering schemes are a hundred apart for the same hit. Acting
        # on first_seq here would select a hit record_value never had.
        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {"label": "buckets", "at": "record_bucket", "capture": ["bucket"]},
                    {"label": "values", "at": "record_value", "capture": ["value"]},
                ],
            }
        )

        outliers = document["aggregate"]["values"]["value"]["outliers"]
        self.assertEqual(len(outliers), 1, str(outliers))
        self.assertEqual(outliers[0]["first_hit"], 43)
        self.assertEqual(outliers[0]["first_seq"], 143)

    def test_process_inputs_reach_the_program(self):
        """args, env, cwd and stdin each arrive, which the program echoes back."""
        self.build()

        working = tempfile.TemporaryDirectory()
        self.addTearDownHook(working.cleanup)
        open(os.path.join(working.name, "cwd_marker"), "w").close()
        stdin_path = os.path.join(working.name, "input.txt")
        with open(stdin_path, "w") as stream:
            stream.write("from-file\n")

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "args": ["probe"],
                "env": {"OBSERVE_ENV": "from-plan"},
                "cwd": working.name,
                "stdin": stdin_path,
                "timeout_seconds": 300,
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        # The echo goes to stdout, and the two streams are reported apart: nothing
        # orders one against the other, so a single buffer would be claiming an
        # interleaving two pipes drained in turn never knew.
        output = document["inferior_output"]["stdout"]
        self.assertIn("mode=probe", output)
        self.assertIn("env=from-plan", output)
        # The marker is opened by a relative path, so finding it proves the
        # working directory took effect rather than the path being absolute.
        self.assertIn("cwd_marker=1", output)
        self.assertIn("stdin=from-file", output)
        self.assertNotIn("stderr", document["inferior_output"], str(document))

    def test_inferior_output_can_be_suppressed(self):
        """capture_inferior_output off leaves the program's own output out."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "capture_inferior_output": False,
                "timeout_seconds": 300,
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        # Absent entirely rather than present and empty, so that its presence is
        # what a caller tests.
        self.assertNotIn("inferior_output", document, str(document))

    def test_called_from_reduces_hits_not_just_events(self):
        """Gating on a caller excludes hits, rather than filtering emission."""
        self.build()

        # leaf is reached three times through gate and twice through ungated.
        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {"label": "all", "at": "leaf", "capture": ["n"]},
                    {
                        "label": "gated",
                        "at": "leaf",
                        "capture": ["n"],
                        "called_from": "gate",
                    },
                ],
            }
        )

        report = document["plan_report"]
        self.assertEqual(report["all"]["hits"], 5, str(report))
        # A hit reached through the other caller is never recorded, so the count
        # itself is lower. Filtering emission would have left hits at five.
        self.assertEqual(report["gated"]["hits"], 3, str(report))

    def test_enabled_after_holds_an_observation_closed(self):
        """An observation waits for its gate, and stays shut without one."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {"label": "gate", "at": "gate", "capture": ["n"]},
                    {
                        "label": "after_gate",
                        "at": "record_value",
                        "capture": ["value"],
                        "enabled_after": "gate",
                    },
                ],
            }
        )

        report = document["plan_report"]
        self.assertEqual(report["gate"]["hits"], 3, str(report))
        # record_value runs a hundred times, but every one of them happens
        # before gate is first reached, so the gated observation never opens.
        self.assertEqual(report["after_gate"]["hits"], 0, str(report))

    def test_return_value_is_recorded_once(self):
        """$return carries the returned value, and only one value per hit."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {
                        "label": "ret",
                        "at": "returns_value",
                        "on": "return",
                        "capture": ["$return", "n"],
                    }
                ],
            }
        )

        summary = document["aggregate"]["ret"]["$return"]
        # Five calls return five distinct values. A sixth would mean the value
        # was recorded twice per hit, once from the ABI and once by resolving
        # the name as if it were a variable.
        self.assertEqual(summary["distinct"], 5, str(summary))
        for rendered in summary["values"]:
            self.assertNotIn("unavailable", rendered, str(summary))

        # A capture other than the return value cannot be read once the frame is
        # gone, and the result says so rather than leaving an empty column.
        self.assertTrue(
            any("frame has already been popped" in note for note in document["notes"]),
            str(document.get("notes")),
        )

    def test_nested_aggregate_is_not_a_cycle(self):
        """A struct's first member starts at its address without being it."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {"label": "deep", "at": "nested", "capture": ["o"], "depth": 4}
                ],
            }
        )

        # A capture with structure keeps it, so the members are read as members
        # rather than looked for as text in an escaped document.
        rendered = document["aggregate"]["deep"]["o"]["value"]
        # o, o->in and o->in.a all begin at one address. Identifying a value by
        # address alone calls the innermost one a cycle and loses it. Checked over
        # the whole value, since a cycle marker would sit at whichever depth the
        # walk gave up at.
        self.assertNotIn("_cycle", json.dumps(rendered), str(rendered))
        self.assertIn("a", rendered["in"], str(rendered))

    def test_depth_bounds_how_far_a_value_is_expanded(self):
        """A deeper limit reaches further into a nested value."""
        self.build()

        plan = {
            "program": self.getBuildArtifact("a.out"),
            "timeout_seconds": 300,
            "observe": [{"label": "deep", "at": "nested", "capture": ["o"]}],
        }

        plan["observe"][0]["depth"] = 1
        shallow = self.observe(plan)["aggregate"]["deep"]["o"]["value"]
        plan["observe"][0]["depth"] = 4
        deep = self.observe(plan)["aggregate"]["deep"]["o"]["value"]

        # Depth one reaches o's own members but not through them.
        self.assertIn("in", shallow, str(shallow))
        self.assertNotIn("a", shallow["in"], str(shallow))
        self.assertIn("a", deep["in"], str(deep))

    def test_backtrace_records_frames_and_collapses_recursion(self):
        """Frames come back per event, with a run of one function counted."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {
                        "label": "rec",
                        "at": "recurse",
                        "capture": ["n"],
                        "backtrace": 8,
                        "only_hit": 5,
                    }
                ],
            }
        )

        report = document["plan_report"]["rec"]
        self.assertEqual(report["hits"], 5, str(report))
        self.assertEqual(report["emitted"], 1, str(report))

        with open(document["artifact"]["path"], "r") as stream:
            events = [json.loads(line) for line in stream.read().splitlines() if line]
        self.assertEqual(len(events), 1, str(events))
        # The per-event backtrace is written under "frames", the same key the
        # terminal event uses for its own, and is advertised in the artifact's
        # field list so a reader does not have to open the file to find it.
        self.assertIn("frames", events[0], str(events[0]))
        self.assertIn("frames", document["artifact"]["fields"], str(document))

    def test_empty_capture_is_a_bare_hit_counter(self):
        """An observation with nothing to read still answers whether code ran."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {"label": "reached", "at": "compute_value"},
                    {"label": "never", "at": "spin"},
                ],
            }
        )

        report = document["plan_report"]
        self.assertEqual(report["reached"]["hits"], 1, str(report))
        # spin exists and resolves, so a zero count here means the code was not
        # reached rather than that the name was wrong.
        self.assertEqual(report["never"]["resolved_locations"], 1, str(report))
        self.assertEqual(report["never"]["hits"], 0, str(report))
        # Nothing was captured, so there is nothing to aggregate.
        self.assertNotIn("aggregate", document)

    def test_skip_first_and_only_hit_compose(self):
        """only_hit is numbered past the hits skip_first ignored."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {
                        "label": "late",
                        "at": "leaf",
                        "capture": ["n"],
                        "skip_first": 2,
                        "only_hit": 4,
                    }
                ],
            }
        )

        report = document["plan_report"]["late"]
        self.assertEqual(report["hits"], 5, str(report))
        self.assertEqual(report["emitted"], 1, str(report))

    def test_condition_and_on_change_compose(self):
        """A false condition suppresses a hit before the mode sees it."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {
                        "label": "rare",
                        "at": "record_value",
                        "capture": ["value"],
                        "when": "value == 99",
                        "emit": "on_change",
                    }
                ],
            }
        )

        report = document["plan_report"]["rare"]
        self.assertEqual(report["hits"], 100, str(report))
        self.assertEqual(report["condition_true"], 1, str(report))
        self.assertEqual(report["condition_errors"], 0, str(report))
        self.assertEqual(report["emitted"], 1, str(report))

    def test_what_a_capture_printed_is_attributed_to_it(self):
        """A capture that runs a printer carries what it printed, per hit and per
        stream, and does not carry the program's own output."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "args": ["printing"],
                "timeout_seconds": 300,
                "observe": [
                    {
                        "label": "printed",
                        "at": "step_printing",
                        "capture": ["describe(n)", "n"],
                    }
                ],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        events = self.events(document)
        self.assertEqual(len(events), 3, str(events))

        for index, event in enumerate(events):
            captured = event["values"]["describe(n)"]
            # A call that ran and returned nothing is not a failure. Its value is
            # void and its answer is what it wrote. Void here rather than the
            # text, because this printer wrote on both streams at once and
            # nothing orders a write on the one against a write on the other --
            # so there is no single text for the value to be.
            self.assertEqual(captured["value"], "(void)", str(captured))
            self.assertNotIn("printed_as_value", captured, str(captured))
            printed = captured["printed"]
            # Attributed per hit: hit 0 does not carry hit 2's line. Standard
            # output is still on a terminal, whose line discipline turns each
            # newline into a carriage return and a newline; standard error is a
            # file of the run's own and is not translated. The bytes are reported
            # as they arrived rather than normalised, since a translation invented
            # here would misreport what the program wrote.
            self.assertEqual(
                printed["stdout"].replace("\r\n", "\n"),
                f"described {index}\n",
                str(printed),
            )
            self.assertEqual(printed["stderr"], f"warned {index}\n", str(printed))

            # The path-tier capture at the same tracepoint runs no code, so it
            # cannot have printed and is not credited with the neighbour that did.
            self.assertNotIn("printed", event["values"]["n"], str(event))

        # The program's own line, written before any tracepoint was hit, is still
        # the program's. A capture's window opens after everything already written
        # has been taken out of the pipe.
        output = document["inferior_output"]
        self.assertIn("described -1", output["stdout"], str(output))
        self.assertIn("warned -1", output["stderr"], str(output))
        # And it stayed out of the captures.
        for event in events:
            self.assertNotIn(
                "-1", event["values"]["describe(n)"]["printed"]["stdout"], str(event)
            )

    def test_what_a_capture_printed_reaches_the_aggregate(self):
        """Printed text is part of the value, so on_change over a printer emits
        when what it prints changes."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "args": ["printing"],
                "timeout_seconds": 300,
                "observe": [
                    {
                        "label": "printed",
                        "at": "step_printing",
                        "capture": ["describe(n)"],
                        "emit": "on_change",
                    }
                ],
            }
        )

        report = document["plan_report"]["printed"]
        self.assertEqual(report["hits"], 3, str(report))
        # Three hits printing three different things are three changes. Keyed on
        # `(void)` alone they would have been one, and the mode would have
        # answered that a dump that changes at every hit never changes.
        self.assertEqual(report["emitted"], 3, str(report))
        aggregate = document["aggregate"]["printed"]["describe(n)"]
        self.assertEqual(aggregate["distinct"], 3, str(aggregate))

    def test_a_printer_on_one_stream_is_aggregated_by_what_it_printed(self):
        """The histogram over a printer counts dumps, not one bucket of void."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "args": ["printing"],
                "timeout_seconds": 300,
                "observe": [
                    {
                        "label": "printed",
                        "at": "step_printing",
                        "capture": ["describe_one_stream(n)"],
                    }
                ],
            }
        )

        events = self.events(document)
        self.assertEqual(len(events), 3, str(events))
        for index, event in enumerate(events):
            captured = event["values"]["describe_one_stream(n)"]
            # The text, not `(void)`, and marked as printed rather than returned:
            # what this expression returned is nothing, which is a different
            # claim about the program from what it printed.
            self.assertEqual(captured["value"], f"node {index}", str(captured))
            self.assertTrue(captured["printed_as_value"], str(captured))
            # Trimmed for the value, verbatim in `printed`. A trailing newline is
            # near-universal in dump output, and left in the value it would make
            # every key differ from its own trimmed form.
            self.assertEqual(
                captured["printed"]["stderr"], f"node {index}\n", str(captured)
            )

        # And the aggregate is keyed on the text: three dumps, each its own value,
        # legible without unescaping a document that wrapped a void marker.
        aggregate = document["aggregate"]["printed"]["describe_one_stream(n)"]
        self.assertEqual(
            aggregate["values"], {"node 0": 1, "node 1": 1, "node 2": 1}, str(aggregate)
        )

    def test_expression_capture_reports_its_tier(self):
        """A capture that is not a path is reported as an expression."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {
                        "label": "computed",
                        "at": "nested",
                        "capture": ["o->in.a + o->in.b"],
                    }
                ],
            }
        )

        captures = document["plan_report"]["computed"]["captures"]
        tier = capture_tier(captures["o->in.a + o->in.b"])
        # Arithmetic has no path form, so it has to reach the expression
        # evaluator, and the report says which mechanism ran.
        self.assertEqual(tier, "expression", str(captures))

    def test_source_location_resolves_and_a_bad_line_does_not(self):
        """A file:line observation resolves, and an uncovered line says why."""
        self.build()
        leaf_line = line_number("main.c", "the leaf body")

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {"label": "by_line", "at": f"main.c:{leaf_line}"},
                    {"label": "no_such_line", "at": "main.c:99999"},
                ],
            }
        )

        report = document["plan_report"]
        self.assertGreaterEqual(report["by_line"]["resolved_locations"], 1, str(report))
        self.assertEqual(report["by_line"]["hits"], 5, str(report))

        missing = report["no_such_line"]
        self.assertEqual(missing["resolved_locations"], 0, str(missing))
        # A near-miss function name explains nothing about a line number, so the
        # message has to talk about line tables instead.
        self.assertIn("line table", missing["error"], missing["error"])

    def test_clean_exit_carries_no_cycle(self):
        """A loop is how programs are written, not evidence of being stuck."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {"label": "loop", "at": "record_value", "capture": ["value"]}
                ],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        self.assertNotIn("cycle", document)

    def test_timeout_is_a_result_with_a_terminal_event(self):
        """A program that never ends is reported, not left to hang."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "args": ["spin"],
                "timeout_seconds": 10,
            }
        )

        self.assertEqual(document["outcome"], "timed_out", str(document))
        terminal = document["terminal"]
        # A timeout is a terminal event and gets a crash's treatment: without a
        # backtrace there is nothing to say about where it got stuck.
        self.assertIn("spin", str(terminal.get("frames")), str(terminal))
        self.assertNotIn("exit_status", terminal)

        # A plan that named nothing still says where the program was. Every other
        # part of a plan answers a question the caller already knew to ask, and
        # for a program pinned to a core the question is which code is running.
        profile = document["profile"]
        self.assertGreater(profile["samples"], 0, str(profile))
        self.assertEqual(profile["hot"][0]["function"], "spin", str(profile))
        self.assertIn("main", profile["under"], str(profile))

        # Sampling means stopping the program, and the stop is delivered as a
        # SIGSTOP: a run that read its own halt as the program dying would report
        # every long run as a crash.
        self.assertEqual(document["outcome"], "timed_out", str(document))

    def test_two_identical_runs_report_nothing_diverged(self):
        """The answer to "did anything move" is a list of what did not."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [{"at": "record_value", "capture": ["value"]}],
                "compare": [{"label": "left"}, {"label": "right"}],
            }
        )

        # One document rather than one per run: a caller handed two reports has to
        # diff them itself, which is the work this exists to do.
        self.assertEqual(len(document["runs"]), 2, str(document))
        self.assertNotIn("diverged", document, str(document))
        self.assertNotIn("first_divergent_hit", document, str(document))
        self.assertIn("record_value.hits", document["agreed"], str(document))
        self.assertIn("outcome", document["agreed"], str(document))

    def test_runs_that_differ_report_what_differed_and_name_the_rest(self):
        """A tracepoint hit in one run and not the other is the commonest
        difference there is, and silence about it would read as agreement."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [{"at": "record_value", "capture": ["value"]}],
                # `churn` returns before the loop that calls record_value, so the
                # tracepoint resolves in both runs and fires in only one.
                "compare": [{"label": "traced"}, {"label": "churned",
                                                  "args": ["churn"]}],
            }
        )

        hits = document["diverged"]["record_value.hits"]
        self.assertEqual(hits["traced"], "100", str(document))
        self.assertEqual(hits["churned"], "0", str(document))

        # The name resolved in both, so that is not what differs -- and saying so
        # is what tells a reader the difference is the code path and not the build.
        self.assertIn("record_value.resolved_locations", document["agreed"],
                      str(document))

        rows = {run["label"]: run for run in document["runs"]}
        self.assertEqual(rows["traced"]["hits"]["record_value"], 100)
        self.assertEqual(rows["churned"]["hits"]["record_value"], 0)

    def test_a_run_that_cannot_be_made_is_a_row_in_the_comparison(self):
        """A change that stops the program from starting is the difference being
        looked for, so the runs that did work still have to answer."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [{"at": "record_value"}],
                "compare": [
                    {"label": "there"},
                    {"label": "gone", "program": self.getBuildArtifact("no.out")},
                ],
            }
        )

        rows = {run["label"]: run for run in document["runs"]}
        self.assertEqual(rows["there"]["outcome"], "exited", str(document))
        self.assertIn("error", rows["gone"], str(document))
        # Nothing is called agreed when one side said nothing at all.
        self.assertNotIn("agreed", document, str(document))

    def test_a_program_that_ends_before_a_sample_is_due_has_no_profile(self):
        """A section that says nothing is worse than no section."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [{"at": "record_value"}],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        self.assertNotIn("profile", document)

    def test_no_progress_is_disarmed_by_default(self):
        """A plan whose trigger fires late is not cut short."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [{"label": "late", "at": "recurse", "capture": ["n"]}],
            }
        )

        # recurse is reached only after two hundred other calls, so a
        # no-progress ceiling that defaulted on would end the run first.
        self.assertEqual(document["outcome"], "exited", str(document))
        self.assertEqual(document["plan_report"]["late"]["hits"], 5)

    def test_no_progress_ends_a_stalled_run(self):
        """Asked for, a stall with no events ends the run and says which way."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "args": ["spin"],
                "timeout_seconds": 300,
                "no_progress_seconds": 10,
                "observe": [{"label": "never", "at": "recurse", "capture": ["n"]}],
            }
        )

        # The spin is reached before recurse, so no event ever arrives and the
        # stall ceiling is what ends the run, well inside the wall-clock one.
        self.assertEqual(document["outcome"], "no_progress", str(document))
        self.assertLess(document["elapsed_ms"], 300000, str(document))

    def test_no_progress_is_measured_over_hits_not_events(self):
        """A mode that emits almost nothing is not a program that has stalled."""
        self.build()

        # churn hits the tracepoint every millisecond for three seconds, while
        # first_and_last emits twice in the whole run. A stall ceiling measured
        # over the event stream would end this run a moment after the first hit
        # and call a healthy program stuck.
        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "args": ["churn"],
                "timeout_seconds": 300,
                "no_progress_seconds": 2,
                "observe": [{"at": "tick", "capture": ["n"], "emit": "first_and_last"}],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        report = document["plan_report"]["tick"]
        self.assertEqual(report["hits"], 3000, str(report))
        # The first hit, and the last one held until the run ended.
        self.assertEqual(report["emitted"], 2, str(report))

        # Every one of those hits carried a value nothing else did, which makes
        # every value rare -- which is to say that none of them is. The claim is
        # withheld rather than bounded, since a list of three thousand equally
        # rare values is the stream this is supposed to stand in for. Withheld out
        # loud, though: an absent claim reads the same as a population that had no
        # outliers, which is the opposite finding. A string rather than an empty
        # array for the same reason -- a client that reads the field as a list gets
        # a type error where it would otherwise conclude nothing rare happened.
        aggregate = document["aggregate"]["tick"]["n"]
        self.assertEqual(aggregate["distinct"], 3000, str(aggregate)[:400])
        self.assertIsInstance(aggregate["outliers"], str, str(aggregate)[:400])
        self.assertTrue(
            aggregate["outliers"].startswith("withheld:"), str(aggregate)[:400]
        )
        self.assertEqual(aggregate["outliers_of"], 3000, str(aggregate)[:400])
        # Nothing was elided: the claim was not made at all rather than shortened.
        self.assertNotIn("outliers_elided", aggregate)

        # Nothing dominates a population that is entirely distinct, so the
        # histogram and the transitions each keep one example and say how many
        # they stood for. The cardinality above is what is not elided.
        self.assertEqual(len(aggregate["values"]), 1, str(aggregate)[:400])
        self.assertEqual(aggregate["values_elided"], 2999, str(aggregate)[:400])
        self.assertEqual(len(aggregate["transitions"]), 1, str(aggregate)[:400])
        self.assertEqual(aggregate["transitions_elided"], 2998, str(aggregate)[:400])

    def test_a_frame_that_never_returns_is_not_a_return(self):
        """A frame left by a longjmp produces no event and is accounted for."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "args": ["unwind"],
                "timeout_seconds": 300,
                "observe": [{"at": "abandons", "on": "return"}],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        report = document["plan_report"]["abandons"]
        # Three calls, one of which leaves through a longjmp.
        self.assertEqual(report["hits"], 3, str(report))
        self.assertEqual(report["returns_abandoned"], 1, str(report))

        # Two returns, and exactly two: the breakpoint at the return address is
        # shared by all three calls, so waiting on the address rather than on the
        # frame would report the third call's return twice -- once for itself and
        # once for the frame that never came back.
        self.assertEqual(report["emitted"], 2, str(report))
        returned = [
            event["values"]["$return"]["value"] for event in self.events(document)
        ]
        self.assertEqual(returned, ["7", "9"], str(returned))

        # The difference between three calls and two returns is otherwise a
        # miscount as far as a reader can tell.
        self.assertTrue(
            any("without returning" in note for note in document["notes"]),
            str(document.get("notes")),
        )

    def test_hits_from_several_threads_are_reported_together(self):
        """Two threads at one tracepoint are counted, and each event says which."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "args": ["threads"],
                "timeout_seconds": 300,
                "observe": [{"at": "shared_step", "capture": ["n"]}],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        report = document["plan_report"]["shared_step"]
        # Five calls on each of two threads, whatever order they interleave in.
        self.assertEqual(report["hits"], 10, str(report))
        self.assertEqual(report["threads"], 2, str(report))

        # Every number in the report covers the observation rather than one
        # thread, so the sequence is an interleaving and the report has to say so
        # rather than leave it to be discovered.
        self.assertTrue(
            any("threads" in note for note in document["notes"]),
            str(document.get("notes")),
        )

        events = self.events(document)
        self.assertEqual(len(events), 10, str(events))
        self.assertEqual(len({event["tid"] for event in events}), 2, str(events))

    def test_a_name_in_a_library_resolves_when_the_library_loads(self):
        """A name that matched nothing before the launch is not left saying so."""
        self.build()

        library = self.getBuildArtifact(
            "libobserve_plugin.dylib"
            if self.platformIsDarwin()
            else "libobserve_plugin.so"
        )
        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "args": ["plugin", library],
                "timeout_seconds": 300,
                "observe": [{"at": "plugin_step", "capture": ["n"]}],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        report = document["plan_report"]["plugin_step"]
        self.assertEqual(report["hits"], 4, str(report))

        # The count is re-read after the run, so the one field that exists to
        # tell a misspelled name from code that never ran cannot report a name
        # that matched nothing next to the hits proving it did.
        self.assertGreaterEqual(report["resolved_locations"], 1, str(report))
        self.assertNotIn("error", report, str(report))

    def test_only_hit_takes_the_number_the_aggregate_reported(self):
        """An outlier's first_hit is numbered the way only_hit reads it."""
        self.build()

        # skip_first is what pulls the two numberings apart: one counts the hits
        # that were recorded, the other counts the observation's own hits. The
        # loop the tool advertises -- read an outlier, re-run for that one hit --
        # lands on a different hit if they disagree.
        plan = {
            "program": self.getBuildArtifact("a.out"),
            "timeout_seconds": 300,
            "observe": [{"at": "record_value", "capture": ["value"], "skip_first": 2}],
        }
        document = self.observe(plan)

        report = document["plan_report"]["record_value"]
        self.assertEqual(report["hits"], 100, str(report))
        self.assertEqual(report["emitted"], 98, str(report))

        outliers = document["aggregate"]["record_value"]["value"]["outliers"]
        self.assertEqual(len(outliers), 1, str(outliers))
        self.assertEqual(serialized_scalar(outliers[0]["value"]), "99")
        # main.c passes 99 at the forty-third call, which is what the number has
        # to name whether or not the first two were skipped.
        self.assertEqual(outliers[0]["first_hit"], 43, str(outliers))

        plan["observe"][0]["only_hit"] = outliers[0]["first_hit"]
        repeat = self.observe(plan)

        record = repeat["plan_report"]["record_value"]
        self.assertEqual(record["hits"], 100, str(record))
        self.assertEqual(record["emitted"], 1, str(record))
        events = self.events(repeat)
        self.assertEqual(len(events), 1, str(events))
        # The one hit that came back is the one the aggregate pointed at.
        self.assertEqual(events[0]["values"]["value"]["value"], "99", str(events))

    def capture_failure(self, document, capture):
        """The top-level failure entry for one capture, asserted to exist."""
        failures = document.get("capture_failures", [])
        for failure in failures:
            if failure["capture"] == capture:
                return failure
        raise AssertionError(
            f"no capture_failures entry for {capture!r} in {failures}"
        )

    def test_a_fixit_is_applied_and_reported_once(self):
        """A dot written on a pointer resolves after the fixit, and the report
        says which spelling actually ran."""
        self.build()

        # `o` is an `Outer *`, so `o.c` is not valid C and the evaluator repairs
        # it to `o->c`. Written as part of a larger expression on purpose: a bare
        # `o.c` never reaches the expression evaluator at all, because the
        # variable-path tier accepts a dot through a pointer and answers it
        # directly. A capture that needs repairing is therefore one that tier
        # cannot answer, and this is the smallest such expression.
        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [{"at": "nested", "capture": ["o.c + 0"]}],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        report = document["plan_report"]["nested"]
        capture = report["captures"]["o.c + 0"]
        self.assertEqual(capture["fixed_as"], "o->c + 0", str(capture))
        self.assertEqual(capture["tier"], "expression", str(capture))
        # One evaluation for the one hit, which is what adopting the repair buys:
        # the failed parse is paid for once rather than at every hit.
        self.assertEqual(capture["evaluations"], report["hits"], str(capture))

        # Keyed on what the caller wrote, in the report and in the aggregate
        # alike, so that a request can be correlated with its histogram.
        self.assertIn("o.c + 0", report["captures"], str(report))
        self.assertNotIn("o->c + 0", report["captures"], str(report))
        aggregate = document["aggregate"]["nested"]
        self.assertIn("o.c + 0", aggregate, str(aggregate))
        self.assertNotIn("o->c + 0", aggregate, str(aggregate))

        # And the capture worked, so it is not a failure.
        self.assertNotIn("capture_failures", document, str(document))
        self.assertEqual(aggregate["o.c + 0"], "3 x1", str(aggregate))

    def test_a_stable_failure_is_reported_and_not_retried(self):
        """A capture naming nothing in scope is reported at top level, disabled,
        and evaluated exactly once however many hits follow."""
        self.build()

        # record_value is hit a hundred times, so a capture still being tried
        # would show it. `valu` is a slip for the parameter `value`.
        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [{"at": "record_value", "capture": ["valu"]}],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        report = document["plan_report"]["record_value"]
        self.assertEqual(report["hits"], 100, str(report))

        # One location, so one program counter, so one scope for the name to be
        # absent from. A name out of scope where the tracepoint has several
        # locations keeps its attempts, because differently inlined copies have
        # different variables in scope -- so this assertion depends on there
        # being exactly one, and says so rather than failing obscurely.
        self.assertEqual(report["resolved_locations"], 1, str(report))

        # The claim is about cost, so it is asserted on the count of evaluations
        # rather than on the wording of the report: one compile, not a hundred.
        capture = report["captures"]["valu"]
        self.assertEqual(capture["evaluations"], 1, str(capture))
        self.assertEqual(capture["errors"], 1, str(capture))
        self.assertIn("disabled", capture, str(capture))

        failure = self.capture_failure(document, "valu")
        self.assertEqual(failure["observation"], "record_value", str(failure))
        self.assertEqual(failure["reason"], "no_such_name", str(failure))
        self.assertTrue(failure["disabled"], str(failure))
        # The compiler's own words, not a paraphrase of them.
        self.assertIn("undeclared identifier", failure["detail"], str(failure))
        # A bad local is answered with the frame's own names.
        self.assertEqual(failure["candidates"][0], "value", str(failure))

    def test_a_bad_member_is_answered_with_the_types_fields(self):
        """A member the type does not have lists the members it does."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [{"at": "nested", "capture": ["o.chidren", "o.nmae"]}],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))

        # `o.chidren` draws both "is a pointer; did you mean '->'" and "no member
        # named", and the member name is the fault: applying the arrow reaches the
        # same type and still finds no such member.
        missing = self.capture_failure(document, "o.chidren")
        self.assertEqual(missing["reason"], "no_such_member", str(missing))
        self.assertEqual(sorted(missing["candidates"]), ["c", "in", "name"])

        # A near miss is ranked ahead of the rest rather than merely included.
        typo = self.capture_failure(document, "o.nmae")
        self.assertEqual(typo["reason"], "no_such_member", str(typo))
        self.assertEqual(typo["candidates"][0], "name", str(typo))

    def test_a_situational_failure_is_kept_and_reported_as_resolved(self):
        """A capture unavailable at some hits and available at others is not
        given up on, and is not reported as a failure."""
        self.build()

        # sometimes_null is called three times with a null pointer and three
        # times with a real one, so the capture fails first and then works.
        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [{"at": "sometimes_null", "capture": ["o->c"]}],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        report = document["plan_report"]["sometimes_null"]
        self.assertEqual(report["hits"], 6, str(report))

        capture = report["captures"]["o->c"]
        # Every hit was tried: giving up here would have lost the readings that
        # worked.
        self.assertEqual(capture["evaluations"], 6, str(capture))
        self.assertNotIn("disabled", capture, str(capture))
        self.assertGreater(capture["errors"], 0, str(capture))

        # Read after the run, the same rule an unresolved tracepoint location
        # follows: a capture that recovered is reported as resolved rather than as
        # the failure its first hit was.
        for failure in document.get("capture_failures", []):
            self.assertNotEqual(failure["capture"], "o->c", str(failure))

        # And the values that were readable are in the aggregate -- only those.
        # The hits where the pointer was null hold no value the program took, so
        # they are not values: they are the same error, reported once, in the
        # capture's own error count. Which leaves one distinct value, and a
        # single-valued capture collapses to text.
        aggregate = document["aggregate"]["sometimes_null"]["o->c"]
        self.assertEqual(aggregate, "3 x4", str(aggregate))

    def test_a_composite_capture_keeps_its_shape(self):
        """A capture with structure comes back as JSON, not as JSON escaped into
        a string, and a capture that could not be read is not a value."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {
                        "label": "shapes",
                        "at": "nested",
                        "capture": ["o->in", "o->nosuch"],
                    }
                ],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        aggregate = document["aggregate"]["shapes"]

        # The struct arrives as a struct. Keyed as text inside the aggregate,
        # because that is what lets an emission mode and a comparison decide "the
        # same value" by comparing strings, but rendered as the document it came
        # from -- as a key it would be escaped by the serialization that writes the
        # response, and a caller would have to undo that by hand to read it.
        entry = aggregate["o->in"]
        self.assertIsInstance(entry, dict, str(entry))
        self.assertEqual(entry["count"], 1, str(entry))
        self.assertEqual(
            entry["value"], {"a": {"value": "1"}, "b": {"value": "2"}}, str(entry)
        )

        # A capture that could not be read is not in the aggregate at all. It is
        # an error about the capture rather than a value the program took, and
        # left in it would compete with the real values for a bounded histogram,
        # report a change the program never made, and turn up as the rare value a
        # caller is told to read first.
        self.assertNotIn("o->nosuch", aggregate, str(aggregate))
        failure = self.capture_failure(document, "o->nosuch")
        self.assertEqual(failure["reason"], "no_such_member", str(failure))

    def test_a_formatter_renders_a_value_and_its_absence_is_reported(self):
        """A type with a formatter comes back as the one thing it stands for, and
        a run that met none says so.

        What loads them is not tested here: the session sources `~/.lldbinit`,
        which is the developer's own file and not a test's to write. What is
        tested is the half that file reaches -- that a formatter present in the
        session decides the rendering, and that a run which found none says so
        rather than leaving a caller to read an expanded struct as the value.
        """
        self.build()

        plan = {
            "program": self.getBuildArtifact("a.out"),
            "timeout_seconds": 300,
            "observe": [{"label": "inner", "at": "nested", "capture": ["o->in"]}],
        }

        # With no formatter for Inner, and nothing else in the run to have one:
        # two integer members and no summary anywhere.
        without = self.observe(plan)
        entry = without["aggregate"]["inner"]["o->in"]
        self.assertEqual(
            entry["value"], {"a": {"value": "1"}, "b": {"value": "2"}}, str(entry)
        )
        notes = " ".join(without.get("notes", []))
        self.assertIn("no data formatter matched", notes, notes)

        self.runCmd('type summary add --summary-string "a=${var.a}" Inner')
        self.addTearDownHook(lambda: self.runCmd("type summary delete Inner"))

        # The summary stands in for the subtree, which is the point of having one:
        # expanding children past a good one costs tokens and adds nothing.
        with_formatter = self.observe(plan)
        entry = with_formatter["aggregate"]["inner"]["o->in"]
        self.assertEqual(entry["value"], {"summary": "a=1"}, str(entry))
        # And the note is gone, because it was never about this type: it says that
        # nothing in the run was rendered by a formatter at all.
        notes = " ".join(with_formatter.get("notes", []))
        self.assertNotIn("no data formatter matched", notes, notes)

    def test_a_run_that_hit_nothing_says_so(self):
        """A plan whose tracepoints all resolved and never fired draws the
        conclusion the per-observation counts leave to the reader."""
        self.build()

        # crash_now is only reached in the crash mode, so in any other run the
        # location resolves and the code is never reached.
        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [{"at": "crash_now", "capture": ["null_pointer"]}],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        report = document["plan_report"]["crash_now"]
        # The numbers that were already there, and are individually true.
        self.assertGreaterEqual(report["resolved_locations"], 1)
        self.assertEqual(report["hits"], 0, str(report))

        notes = " ".join(document.get("notes", []))
        self.assertIn("no tracepoint in this plan was hit", notes, notes)
        # Named, because a caller that has just spent a whole timeout arriving
        # here has no other way to find it. Not given a value: nothing in the run
        # knows whether these tracepoints were late or unreachable.
        self.assertIn("no_progress_seconds", notes, notes)

        # And not said when nothing resolved: there the observation's own error
        # names what matched no code and suggests the nearest name that would
        # have, which is the better answer and already reported.
        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [{"at": "compute_valu"}],
            }
        )
        notes = " ".join(document.get("notes", []))
        self.assertNotIn("no tracepoint in this plan was hit", notes, notes)

        # Nor when a tracepoint was hit, which is the case that has to be read
        # from the same place the hit counts come from. Reading it from the
        # reports instead said "nothing was hit" on a run whose own plan_report
        # said a hundred hits, because the reports are filled in afterwards.
        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [{"at": "record_bucket", "capture": ["bucket"]}],
            }
        )
        self.assertEqual(document["plan_report"]["record_bucket"]["hits"], 100)
        notes = " ".join(document.get("notes", []))
        self.assertNotIn("no tracepoint in this plan was hit", notes, notes)

    def test_a_condition_that_cannot_be_evaluated_says_so(self):
        """A `when` that never resolves is reported as a condition, not left as
        an observation that recorded nothing."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {"at": "record_value", "when": "valu == 7", "capture": ["value"]}
                ],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        report = document["plan_report"]["record_value"]
        # The counts that already existed: the code ran, and the condition was
        # never once evaluable.
        self.assertEqual(report["hits"], 100, str(report))
        self.assertEqual(report["condition_true"], 0, str(report))
        self.assertEqual(report["condition_errors"], 100, str(report))

        # None of which says the condition was the problem, which is what the
        # top-level entry is for.
        failure = self.capture_failure(document, "valu == 7")
        self.assertEqual(failure["field"], "when", str(failure))
        self.assertEqual(failure["reason"], "no_such_name", str(failure))
        # A condition is never turned off: a run that stopped evaluating it would
        # be recording different hits rather than fewer.
        self.assertFalse(failure["disabled"], str(failure))

    # A tracepoint's own work compiled into the program rather than done at a
    # stop. The subject is `accumulate`, which is several lines long: the work
    # stands where a statement stands, and a one-line function has nowhere to put
    # it. Only arm64 has an entry trampoline to redirect with, and the fallback
    # everywhere else is the stopping path these tests exist to distinguish from.
    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_two_tracepoints_in_one_function_share_its_copy(self):
        """A plan's tracepoints in one function are compiled into one copy of it.

        A compile runs clang over the whole function body and JITs the result
        into the program, and each one retires a copy the target keeps for the
        rest of its life -- leaving a location behind in every breakpoint over
        those lines. So a plan is compiled a function at a time rather than a
        tracepoint at a time, and every tracepoint of a function ends up in the
        same copy.

        What is asserted is the consequence: both of these are in the program, and
        each counts its own hits. A plan that compiled per tracepoint would publish
        one copy per tracepoint, and only the last one written would be reached.
        """
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {"label": "never", "at": "accumulate", "when": "seed > 1000"},
                    {"label": "twice", "at": "accumulate", "when": "rounds == 2"},
                ],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        never = document["plan_report"]["never"]
        twice = document["plan_report"]["twice"]
        self.assertEqual(never["eval"], "in-process", str(never))
        self.assertEqual(twice["eval"], "in-process", str(twice))

        # Both saw every call, because both are in the copy the entry reaches.
        self.assertEqual(never["hits"], 25, str(never))
        self.assertEqual(twice["hits"], 25, str(twice))
        # And each tested its own condition: only accumulate_via passes two.
        self.assertEqual(never["condition_true"], 0, str(never))
        self.assertEqual(twice["condition_true"], 5, str(twice))

        # One recompile, so the note names the function once.
        notes = " ".join(document.get("notes", []))
        self.assertIn("accumulate", notes, notes)

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_a_patched_frame_reports_only_the_programs_own_locals(self):
        """A frame of a patched function is still the program's frame.

        The code compiled into a function declares working locals of its own --
        the hit counter every injection opens with, and one per capture -- and
        they stand in the frame beside the program's. Measured: with nothing
        filtering them, a terminal event inside a patched function reported
        `__lldb_h_1_0` to the caller as one of the locals the program had, and
        counted it again in the note saying how many were not read. Neither is
        something a caller can act on: the name is the debugger's, and the next
        recompile gives it a different one.

        The tracepoint goes inside the spin loop rather than at the function,
        because the condition has to name something -- a constant one folds away
        before it reaches the copy, leaving no trap for the site to attribute.
        """
        self.build()
        # The body of the endless loop, where the loop counter is in scope.
        body = line_number("main.c", "for (volatile long i") + 1

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "args": ["spin"],
                "timeout_seconds": 6,
                "observe": [
                    {
                        "label": "spinning",
                        "at": "main.c:%d" % body,
                        "when": "i > 100000000000",
                    }
                ],
            }
        )

        self.assertEqual(document["outcome"], "timed_out", str(document))
        report = document["plan_report"]["spinning"]
        self.assertEqual(report["eval"], "in-process", str(report))

        # The ceiling stopped the program inside the copy, which is what puts the
        # injection's own locals in the frame the terminal event reports.
        terminal = document["terminal"]
        self.assertEqual(terminal["function"], "spin", str(terminal))
        locals_reported = terminal["locals"]
        self.assertIn("i", locals_reported, str(locals_reported))
        for name in locals_reported:
            self.assertFalse(
                name.lower().startswith("__lldb"),
                "%r is the debugger's own: %s" % (name, locals_reported),
            )
        # And nothing says a local was withheld, because none was: the one that is
        # missing was never the program's to report.
        self.assertNotIn("_elided", locals_reported, str(locals_reported))

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_a_condition_runs_in_process_by_default(self):
        """A plan's condition is compiled into the program unless refused.

        The counts are the whole assertion: a condition that never holds takes no
        stop at all, so the twenty-five hits reported here were counted by the
        program and read back out of it rather than seen by the debugger.
        """
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [{"at": "accumulate", "when": "seed > 1000"}],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        report = document["plan_report"]["accumulate"]
        self.assertEqual(report["eval"], "in-process", str(report))
        # main.c calls it twenty times directly and five times through
        # accumulate_via.
        self.assertEqual(report["hits"], 25, str(report))
        self.assertEqual(report["condition_true"], 0, str(report))
        self.assertEqual(report["condition_errors"], 0, str(report))
        # Nothing was spent evaluating it at a stop, because nothing stopped.
        self.assertEqual(report["condition_ms"], 0.0, str(report))
        self.assertEqual(report["emitted"], 0, str(report))

        # The program really is running an unoptimized copy of the function, and
        # that is a property of the run rather than of the observation.
        notes = " ".join(document.get("notes", []))
        self.assertIn("accumulate", notes, notes)
        self.assertIn("without optimization", notes, notes)

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_fast_false_uses_the_stopping_path(self):
        """The escape hatch removes the variable for anyone who needs it.

        A patched function is a recompile, so somebody measuring the program's own
        timing has to be able to ask for the program as it was built.
        """
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "fast": False,
                "timeout_seconds": 300,
                "observe": [{"at": "accumulate", "when": "seed > 1000"}],
            }
        )

        report = document["plan_report"]["accumulate"]
        self.assertTrue(report["eval"].startswith("stopped"), str(report))
        # The same answer, at a stop per hit.
        self.assertEqual(report["hits"], 25, str(report))
        self.assertEqual(report["condition_true"], 0, str(report))
        self.assertEqual(report["condition_errors"], 0, str(report))
        # Nothing was recompiled, so nothing says anything was.
        notes = " ".join(document.get("notes", []))
        self.assertNotIn("without optimization", notes, notes)

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_a_condition_and_its_captures_both_go_into_the_program(self):
        """A hit whose condition holds is recorded rather than stopped for.

        The hits the condition excluded cost nothing, and the one it let through
        costs nothing either: its values were written where the program held them.
        They are the values the stopping path reads, which is what makes the two
        modes comparable rather than merely both cheap.
        """
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {
                        "at": "accumulate",
                        "when": "seed == 7",
                        "capture": ["seed", "rounds"],
                    }
                ],
            }
        )

        report = document["plan_report"]["accumulate"]
        self.assertEqual(report["eval"], "in-process", str(report))
        self.assertEqual(report["hits"], 25, str(report))
        # main.c passes each of 0..19 once, so the seventh call is the only one.
        self.assertEqual(report["condition_true"], 1, str(report))
        self.assertEqual(report["emitted"], 1, str(report))
        for name in ("seed", "rounds"):
            capture = report["captures"][name]
            self.assertEqual(capture_tier(capture), "in_process", str(capture))
            self.assertEqual(capture_reads(capture), 1, str(capture))

        # One distinct value apiece, so each collapses to text -- and the values
        # are the ones the seventh call held.
        values = document["aggregate"]["accumulate"]
        self.assertEqual(values["seed"], "7 x1", str(values))
        self.assertEqual(values["rounds"], "3 x1", str(values))

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_a_gated_condition_is_opened_and_closed_in_the_program(self):
        """A gate the debugger opens reaches the code compiled into the program.

        Without it the gate would be a breakpoint nobody arms any more: the trap
        is in the copy, and disabling a breakpoint does not reach it. The hit
        count is the assertion -- twenty calls happen with the gate shut.
        """
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {
                        "at": "accumulate",
                        "when": "rounds == 2",
                        "called_from": "accumulate_via",
                    }
                ],
            }
        )

        report = document["plan_report"]["accumulate"]
        self.assertEqual(report["eval"], "in-process", str(report))
        # Only the five calls reached through accumulate_via are this
        # observation's hits; the twenty direct ones happen with the gate shut.
        self.assertEqual(report["hits"], 5, str(report))
        self.assertEqual(report["condition_true"], 5, str(report))
        self.assertEqual(report["emitted"], 5, str(report))

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_on_return_falls_back_and_says_so(self):
        """A refusal names itself rather than being silent.

        Which mode an observation got has to be readable, because a caller
        comparing hit counts between runs is comparing what each of them paid.
        """
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {
                        "at": "accumulate",
                        "on": "return",
                        "when": "1 == 2",
                        "capture": ["$return"],
                    }
                ],
            }
        )

        report = document["plan_report"]["accumulate"]
        self.assertTrue(report["eval"].startswith("stopped:"), str(report))
        self.assertIn("returns", report["eval"], str(report))
        # The observation went on working, which is what makes the fallback a
        # fallback rather than a failure.
        self.assertEqual(report["hits"], 25, str(report))
        self.assertEqual(report["condition_true"], 0, str(report))

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_a_condition_that_cannot_be_compiled_says_why(self):
        """A condition the compiler rejected falls back with the diagnostic.

        The condition still works -- it is evaluated at a stop, once per hit --
        and the cost of that is the whole reason the reason has to be readable.
        """
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [{"at": "accumulate", "when": "valu == 7"}],
            }
        )

        report = document["plan_report"]["accumulate"]
        self.assertTrue(report["eval"].startswith("stopped:"), str(report))
        self.assertIn("did not compile", report["eval"], str(report))
        # And the compiler's own words for it, which are the actionable half.
        self.assertIn("valu", report["eval"], str(report))
        # Evaluated at a stop instead, and it fails there too -- which is a
        # different report, and one that was already being made.
        self.assertEqual(report["hits"], 25, str(report))
        self.assertEqual(report["condition_errors"], 25, str(report))
        failure = self.capture_failure(document, "valu == 7")
        self.assertEqual(failure["field"], "when", str(failure))

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_a_hot_capture_costs_no_stop_per_hit(self):
        """The claim the whole feature is for, made where it can be seen.

        Twenty thousand hits under a five-second ceiling. A stop per hit does not
        fit in five seconds -- one costs about a millisecond, so twenty thousand
        cost twenty seconds -- so the run reaching the program's own end is the
        claim, and every value arriving with it is that nothing was traded for
        it. Written against a hot function because at twenty-five hits the two
        modes are indistinguishable.
        """
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "args": ["hot"],
                "timeout_seconds": 5,
                "observe": [
                    {"at": "hot_step", "capture": ["bucket"], "emit": "on_change"}
                ],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        report = document["plan_report"]["hot_step"]
        self.assertEqual(report["eval"], "in-process", str(report))
        self.assertEqual(report["hits"], 20000, str(report))

        # Every one of them read, and by the program: the ring fills roughly six
        # times over twenty thousand records, so the stops this cost are six
        # rather than twenty thousand.
        capture = report["captures"]["bucket"]
        self.assertEqual(capture_tier(capture), "in_process", str(capture))
        self.assertEqual(capture_reads(capture), 20000, str(capture))

        # Ten thousand hits either side of the bucket boundary, which is every
        # value accounted for rather than a sample of them.
        values = document["aggregate"]["hot_step"]["bucket"]
        self.assertEqual(values["values"], {"0": 10000, "1": 10000}, str(values))
        # And one change between them, so the twenty thousand arrived in order.
        self.assertEqual(report["emitted"], 2, str(report))

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_captures_alone_are_recorded_without_stopping(self):
        """A tracepoint that only reads values costs no stop at all.

        This is the case the feature exists for and the one a condition cannot
        help with: with nothing to be false, every hit was a stop, and a plan that
        reads a value at a hot function paid for one at every one of them. The
        values are the assertion -- the same ones a stop would have read, and the
        word on each capture says the program recorded them itself.
        """
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [{"at": "accumulate", "capture": ["seed", "rounds"]}],
            }
        )

        self.assertEqual(document["outcome"], "exited", str(document))
        report = document["plan_report"]["accumulate"]
        self.assertEqual(report["eval"], "in-process", str(report))
        self.assertEqual(report["hits"], 25, str(report))
        self.assertEqual(report["emitted"], 25, str(report))

        # Read at all twenty-five, and by the program rather than at a stop.
        for name in ("seed", "rounds"):
            capture = report["captures"][name]
            self.assertEqual(capture_tier(capture), "in_process", str(capture))
            self.assertEqual(capture_reads(capture), 25, str(capture))

        # main.c passes 0..19 with three rounds and 0..4 with two, which is the
        # distribution a stop would have reported -- and the single transition
        # from three rounds to two, at the twenty-first hit, is the hits having
        # reached the report in the order the program took them.
        values = document["aggregate"]["accumulate"]
        self.assertEqual(values["rounds"]["values"], {"3": 20, "2": 5}, str(values))
        self.assertEqual(
            values["rounds"]["transitions"],
            [{"count": 1, "first_seq": 21, "from": "3", "to": "2"}],
            str(values),
        )
        self.assertEqual(values["seed"]["distinct"], 20, str(values))

        # A hit nothing stopped for was never current anywhere, so it has no
        # thread and no time of its own. Said once for the run rather than left
        # to be discovered from the events.
        notes = " ".join(document.get("notes", []))
        self.assertIn("in_process", notes, notes)
        self.assertIn("t_ms", notes, notes)
        events = self.events(document)
        self.assertEqual(len(events), 25, str(events[:3]))
        self.assertNotIn("t_ms", events[0], str(events[0]))
        self.assertNotIn("tid", events[0], str(events[0]))
        # But the function is known: it is the one that was recompiled.
        self.assertEqual(events[0]["frame"], "accumulate", str(events[0]))

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_a_recorded_hit_keeps_each_captures_own_value(self):
        """Two captures at one hit come back as that hit's pair, not as a shuffle.

        Each value travels with the hit its own counter numbered, which is what
        makes the pair a pair. `on_change` is the assertion: it compares one hit's
        tuple against the previous hit's, so a run whose values had been joined
        across hits would collapse differently -- and `accumulate`'s two arguments
        move independently, the seed at every call and the rounds only twice.
        """
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {
                        "at": "accumulate",
                        "capture": ["rounds", "seed"],
                        "emit": "on_change",
                    }
                ],
            }
        )

        report = document["plan_report"]["accumulate"]
        self.assertEqual(report["eval"], "in-process", str(report))
        self.assertEqual(report["hits"], 25, str(report))
        # The seed changes at every call, so every hit is a change.
        self.assertEqual(report["emitted"], 25, str(report))

        # The pairs themselves: three rounds for the first twenty calls and two
        # for the last five, each beside the seed that call was made with.
        events = self.events(document)
        self.assertEqual(len(events), 25, str(events[:3]))
        for index, event in enumerate(events):
            expected_rounds = 3 if index < 20 else 2
            expected_seed = index if index < 20 else index - 20
            self.assertEqual(
                (event["values"]["rounds"]["value"], event["values"]["seed"]["value"]),
                (str(expected_rounds), str(expected_seed)),
                str(event),
            )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_a_capture_that_needs_a_frame_keeps_its_stop_and_says_so(self):
        """A backtrace is the stack the hit happened on, so the hit has to stop.

        And with nothing else to compile in, the observation gets the stopping
        path whole rather than half of each -- said as the reason, because the
        difference is what the run cost.
        """
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {"at": "accumulate", "capture": ["seed"], "backtrace": 3}
                ],
            }
        )

        report = document["plan_report"]["accumulate"]
        self.assertTrue(report["eval"].startswith("stopped:"), str(report))
        self.assertIn("backtrace", report["eval"], str(report))
        # The observation went on working, which is what makes this a fallback.
        self.assertEqual(report["hits"], 25, str(report))
        self.assertEqual(capture_reads(report["captures"]["seed"]), 25, str(report))
        self.assertEqual(capture_tier(report["captures"]["seed"]), "path", str(report))
        self.assertIn("frames", self.events(document)[0], str(report))

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_a_capture_the_program_cannot_record_keeps_the_whole_stop(self):
        """A value that does not fit a record is read at a stop, and so is its
        neighbour.

        Dropping the one that does not fit would report a hit with a value the
        caller asked for silently missing, and reading the rest at the stop the
        remaining one needs anyway costs nothing beyond what it already costs. So
        the whole observation falls back, and says which value did it.
        """
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                # `pair` is a struct: a record carries eight bytes of scalar.
                "observe": [{"at": "accumulate", "capture": ["seed", "pair"]}],
            }
        )

        report = document["plan_report"]["accumulate"]
        self.assertTrue(report["eval"].startswith("stopped:"), str(report))
        self.assertIn("pair", report["eval"], str(report))
        # Both values still arrive, at a stop per hit.
        self.assertEqual(report["hits"], 25, str(report))
        for name in ("seed", "pair"):
            capture = report["captures"][name]
            self.assertNotEqual(capture_tier(capture), "in_process", str(capture))
            self.assertEqual(capture_reads(capture), 25, str(capture))

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_called_from_with_captures_keeps_the_stop_that_answers_it(self):
        """A gate byte cannot say which thread is inside the gating function.

        `called_from` means a frame of that function is below this hit on this
        thread, and only a stop can answer that. While the hit still stops the
        gate is an optimization on top of the real test; for a hit nobody sees it
        would become the whole of it, and admit another thread's hits silently.
        The count is the assertion -- five of the twenty-five calls are reached
        through `accumulate_via`.
        """
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "timeout_seconds": 300,
                "observe": [
                    {
                        "at": "accumulate",
                        "capture": ["seed"],
                        "called_from": "accumulate_via",
                    }
                ],
            }
        )

        report = document["plan_report"]["accumulate"]
        self.assertTrue(report["eval"].startswith("stopped:"), str(report))
        self.assertIn("called_from", report["eval"], str(report))
        self.assertEqual(report["hits"], 5, str(report))
        self.assertEqual(capture_reads(report["captures"]["seed"]), 5, str(report))

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_fast_false_stops_for_captures_too(self):
        """The escape hatch reaches the captures, not only the conditions."""
        self.build()

        document = self.observe(
            {
                "program": self.getBuildArtifact("a.out"),
                "fast": False,
                "timeout_seconds": 300,
                "observe": [{"at": "accumulate", "capture": ["seed"]}],
            }
        )

        report = document["plan_report"]["accumulate"]
        self.assertTrue(report["eval"].startswith("stopped"), str(report))
        # The same answer, at a stop per hit.
        self.assertEqual(report["hits"], 25, str(report))
        self.assertEqual(capture_reads(report["captures"]["seed"]), 25, str(report))
        self.assertEqual(capture_tier(report["captures"]["seed"]), "path", str(report))
        self.assertIn("t_ms", self.events(document)[0], str(report))
