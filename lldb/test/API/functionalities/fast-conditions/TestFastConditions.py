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

# Launching and exiting cost a few stops of their own, and the condition is
# compiled in at a stop rather than before the program runs, so the first few
# hits are still paid for. A run that evaluated the condition at a stop would
# report at least CALLS on top of that, so anything in this range means the
# condition stopped being evaluated at a stop.
STOPS_ALLOWANCE = 100

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
            process.GetStopID(True), STOPS_ALLOWANCE, "the run paid for no hits"
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

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_a_refusal_says_why(self):
        """A condition that could not be compiled in reports the reason.

        A refusal never stops a condition working -- it goes back to being
        evaluated at a stop -- so nothing visibly fails. What it costs is a stop
        on every hit, which on a hot line is the difference between a run of
        seconds and one of tens of minutes. Somebody who asked for a compiled-in
        condition and quietly got the slow path has nothing to act on, so the
        reason has to be readable.
        """
        target = self.setup()
        # Stopped in the program first. Nothing is compiled in until the dynamic
        # loader has finished starting up, and until then there is nothing to
        # refuse: waiting is not refusing.
        target.BreakpointCreateBySourceRegex(
            r"sum \+= accumulate", lldb.SBFileSpec("main.c")
        )
        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        self.assertState(process.GetState(), lldb.eStateStopped)

        # A libc function was compiled from source this machine does not have, so
        # there is no body to recompile with a condition in it. That is a fact
        # about the program rather than about the moment, which is what makes it
        # a refusal.
        refused = target.BreakpointCreateByName("printf")
        self.assertGreater(refused.GetNumLocations(), 0, "printf resolved")
        refused.SetCondition("1 == 2")
        self.expect(
            "breakpoint list %d" % refused.GetID(),
            substrs=[
                "Condition not compiled into the process: no debug info "
                "describes a function and line at the location"
            ],
        )

        # The same setting and the same moment, for a function whose source is
        # right here: nothing to report.
        compiled_in = target.BreakpointCreateBySourceRegex(
            CONDITION_LINE, lldb.SBFileSpec("main.c")
        )
        compiled_in.SetCondition("seed > %d" % (10 * CALLS))
        self.expect(
            "breakpoint list %d" % compiled_in.GetID(),
            substrs=["Condition not compiled into the process"],
            matching=False,
        )
