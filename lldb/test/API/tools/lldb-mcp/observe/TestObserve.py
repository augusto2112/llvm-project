"""
Test the MCP `observe` tool end to end: a program run under an observation plan,
reported as one JSON document plus a JSONL event artifact.
"""

import json
import os
import shutil
import socket
import tempfile

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


def capture_tier(capture):
    """The tier out of a rendered capture report.

    A capture that resolved as a path and never failed collapses to the tier
    alone, since that is all it has to say.
    """
    if isinstance(capture, str):
        return capture
    return capture["tier"]


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
                "name": "observe",
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

    def events(self, document):
        """The artifact's events, one per line, as objects."""
        path = document["artifact"]["path"]
        with open(path, "r") as stream:
            return [json.loads(line) for line in stream.read().splitlines() if line]

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
        # transitions are numbered by hit.
        transitions = [
            (
                transition["seq"],
                serialized_scalar(transition["from"]),
                serialized_scalar(transition["to"]),
            )
            for transition in aggregate["transitions"]
        ]
        self.assertEqual(transitions, [(26, "0", "1"), (51, "1", "2"), (76, "2", "3")])

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

        # A hit whose condition was false is not captured either, so the
        # capture reports no evaluations rather than a hundred failures.
        capture = report["captures"]["value"]
        self.assertEqual(capture_tier(capture), "unavailable")
        self.assertEqual(capture["evaluations"], 0)

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
        output = document["inferior_output"]
        self.assertIn("mode=probe", output)
        self.assertIn("env=from-plan", output)
        # The marker is opened by a relative path, so finding it proves the
        # working directory took effect rather than the path being absolute.
        self.assertIn("cwd_marker=1", output)
        self.assertIn("stdin=from-file", output)

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
        self.assertNotIn("total=", document.get("inferior_output", ""))

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

        rendered = document["aggregate"]["deep"]["o"]
        # o, o->in and o->in.a all begin at one address. Identifying a value by
        # address alone calls the innermost one a cycle and loses it.
        self.assertNotIn("_cycle", rendered, rendered)
        self.assertIn('"a"', rendered, rendered)

    def test_depth_bounds_how_far_a_value_is_expanded(self):
        """A deeper limit reaches further into a nested value."""
        self.build()

        plan = {
            "program": self.getBuildArtifact("a.out"),
            "timeout_seconds": 300,
            "observe": [{"label": "deep", "at": "nested", "capture": ["o"]}],
        }

        plan["observe"][0]["depth"] = 1
        shallow = self.observe(plan)["aggregate"]["deep"]["o"]
        plan["observe"][0]["depth"] = 4
        deep = self.observe(plan)["aggregate"]["deep"]["o"]

        # Depth one reaches o's own members but not through them.
        self.assertIn('"in"', shallow, shallow)
        self.assertNotIn('"a"', shallow, shallow)
        self.assertIn('"a"', deep, deep)

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
        self.assertIn("backtrace", events[0], str(events[0]))

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
        # every value rare. The outlier list is bounded like the histogram, so a
        # capture shaped like this cannot put one entry per hit in the response.
        aggregate = document["aggregate"]["tick"]["n"]
        self.assertEqual(aggregate["distinct"], 3000, str(aggregate)[:400])
        self.assertEqual(len(aggregate["outliers"]), 32, str(aggregate)[:400])
        self.assertEqual(aggregate["outliers_elided"], 3000 - 32, str(aggregate)[:400])

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
