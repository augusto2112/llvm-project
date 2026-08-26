"""
A breakpoint condition compiled into the process, so that a condition which does
not hold costs no stop.
"""

import os

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil

# The program calls the observed function this many times.
CALLS = 100000

# Launching and exiting costs a few stops of its own that have nothing to do
# with the breakpoint. A run that evaluated the condition at a stop would report
# at least CALLS on top of them, so anything in this range means the condition
# was not evaluated at a stop.
LAUNCH_STOPS = 100

# The line the condition is attached to, which is the loop body of accumulate().
CONDITION_LINE = r"total \+= i;"


class FastConditionsTestCase(TestBase):
    NO_DEBUG_INFO_TESTCASE = True

    def setup(self):
        self.build()
        target = self.dbg.CreateTarget(self.getBuildArtifact("a.out"))
        self.assertTrue(target, VALID_TARGET)
        self.runCmd("settings set target.experimental.fast-conditions true")
        return target

    def get_source_line(self, line):
        with open(os.path.join(self.getSourceDir(), "main.c")) as f:
            return f.readlines()[line - 1]

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_false_condition_never_stops(self):
        """A condition that never holds must not stop the process at all.

        This is the whole point of the feature, so it is the first thing
        asserted: with the condition evaluated at a stop, this run would pay
        100,000 stops.
        """
        target = self.setup()
        # The seed equals the loop counter, so it reaches CALLS - 1 at most and
        # this condition can never hold.
        bp = target.BreakpointCreateBySourceRegex(
            CONDITION_LINE, lldb.SBFileSpec("main.c")
        )
        self.assertGreater(bp.GetNumLocations(), 0, "condition location resolved")
        bp.SetCondition("seed > 1000000")

        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        self.assertState(process.GetState(), lldb.eStateExited)
        self.assertEqual(process.GetExitStatus(), 0)
        self.assertEqual(bp.GetHitCount(), 0, "no hit was reported")
        # A hit count of zero does not on its own say the condition was not
        # evaluated at a stop: a condition that fails takes its own hit back, so
        # the old path also ends at zero -- after paying for CALLS stops. The
        # stop count is what tells the two apart.
        self.assertLess(
            process.GetStopID(True), LAUNCH_STOPS, "the run paid for no hits"
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_true_condition_stops_at_the_right_line(self):
        """A condition that holds stops, reporting the original source."""
        target = self.setup()
        bp = target.BreakpointCreateBySourceRegex(
            CONDITION_LINE, lldb.SBFileSpec("main.c")
        )
        bp.SetCondition("seed == 40000 && i == 2")

        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        self.assertState(process.GetState(), lldb.eStateStopped)

        thread = process.GetSelectedThread()
        frame = thread.GetFrameAtIndex(0)
        # The line comes from the copy's own line table, which the #line
        # directives point back at the original file.
        line_entry = frame.GetLineEntry()
        self.assertEqual(line_entry.GetFileSpec().GetFilename(), "main.c")
        self.assertIn("total += i;", self.get_source_line(line_entry.GetLine()))
        # The locals are the patched function's own, so they read normally.
        self.assertEqual(frame.FindVariable("seed").GetValueAsSigned(), 40000)
        self.assertEqual(frame.FindVariable("i").GetValueAsSigned(), 2)
        # The caller is intact, which is what the entry redirect preserves.
        self.assertEqual(thread.GetFrameAtIndex(1).GetFunctionName(), "main")

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_an_ordinary_breakpoint_on_the_patched_line_still_fires(self):
        """A plain breakpoint on the same line as a fast condition still works.

        The patched copy's debug info points back at the original source, so
        lldb re-resolves file:line breakpoints into it -- which puts an ordinary
        breakpoint at very nearly the address the compiled-in trap is attributed
        to. A site either causes a trap or attributes one, so the two cannot
        share an address, and whichever loses must fail visibly rather than look
        set while never firing.
        """
        target = self.setup()
        conditional = target.BreakpointCreateBySourceRegex(
            CONDITION_LINE, lldb.SBFileSpec("main.c")
        )
        conditional.SetCondition("seed == 50000 && i == 1")
        plain = target.BreakpointCreateBySourceRegex(
            CONDITION_LINE, lldb.SBFileSpec("main.c")
        )

        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        self.assertState(process.GetState(), lldb.eStateStopped)
        # The unconditional breakpoint is hit on the very first call, long
        # before the condition could hold.
        self.assertEqual(plain.GetHitCount(), 1)
        self.assertEqual(conditional.GetHitCount(), 0)

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_falls_back_without_the_setting(self):
        """With the setting off, nothing is patched and the condition still works.

        The fallback is the existing path, so this asserts the feature is opt-in
        rather than asserting how the condition was evaluated.
        """
        self.build()
        target = self.dbg.CreateTarget(self.getBuildArtifact("a.out"))
        self.runCmd("settings set target.experimental.fast-conditions false")
        bp = target.BreakpointCreateBySourceRegex(
            CONDITION_LINE, lldb.SBFileSpec("main.c")
        )
        bp.SetCondition("seed == 5 && i == 1")
        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        self.assertState(process.GetState(), lldb.eStateStopped)
        self.assertEqual(
            process.GetSelectedThread()
            .GetFrameAtIndex(0)
            .FindVariable("seed")
            .GetValueAsSigned(),
            5,
        )
