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
