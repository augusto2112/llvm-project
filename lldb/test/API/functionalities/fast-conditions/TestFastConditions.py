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

# A second line of the same function, for a second condition on it.
SECOND_CONDITION_LINE = "return total;"


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
    def test_a_source_regex_breakpoint_in_the_function_refuses_the_condition(self):
        """A source-regex breakpoint in the function makes the condition refuse.

        Of the two ways to keep such a breakpoint working, this asserts the
        refusal: nothing is patched, the breakpoint goes on firing, and the
        condition says why it is being evaluated at a stop after all.

        The patched copy's line table names the original source, so a breakpoint
        that resolves by file and line finds the copy and survives the redirect.
        A source-regex breakpoint does not: its resolver searches the text of the
        compile unit's primary file, which for the copy is the generated source
        the regex was never written against. Its location stays in a body that no
        longer runs, and a breakpoint that reads as resolved and never fires
        again is the one outcome a patch may not produce.
        """
        target = self.setup()
        # A different line of the same function, since what the redirect makes
        # unreachable is the whole body rather than the line being patched.
        plain = target.BreakpointCreateBySourceRegex(
            SECOND_CONDITION_LINE, lldb.SBFileSpec("main.c")
        )
        self.assertEqual(plain.GetNumLocations(), 1, "plain location resolved")
        fast = target.BreakpointCreateBySourceRegex(
            CONDITION_LINE, lldb.SBFileSpec("main.c")
        )
        # The seed only ever reaches CALLS - 1, so this never holds: with the
        # condition compiled in, nothing would stop the run at all.
        fast.SetCondition("seed > %d" % (10 * CALLS))

        process = target.LaunchSimple(None, None, self.get_process_working_directory())

        # Refused at the first stop, which is the first moment a patch could
        # have been installed, and named: taking the breakpoint that stands in
        # the way off the function is the only way past this refusal.
        self.assertState(process.GetState(), lldb.eStateStopped)
        self.expect(
            "breakpoint list %d" % fast.GetID(),
            substrs=[
                "Condition not compiled into the process: a breakpoint in the "
                "function would stop firing once its entry is redirected to a "
                "copy: breakpoint %d.1" % plain.GetID()
            ],
        )

        # Call after call, which is what says the original body is still the
        # code being run. A patched function would have run to exit instead.
        for expected_seed in range(3):
            self.assertState(process.GetState(), lldb.eStateStopped)
            frame = process.GetSelectedThread().GetFrameAtIndex(0)
            self.assertEqual(
                frame.FindVariable("seed").GetValueAsSigned(), expected_seed
            )
            self.assertEqual(plain.GetHitCount(), expected_seed + 1)
            process.Continue()

        self.assertEqual(fast.GetHitCount(), 0, "the condition never held")

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_two_conditions_in_one_function(self):
        """A second condition on a patched function keeps the first one working.

        Both end up compiled into the same copy, because the copy is rebuilt
        from the original source rather than patched again.
        """
        target = self.setup()
        first = target.BreakpointCreateBySourceRegex(
            CONDITION_LINE, lldb.SBFileSpec("main.c")
        )
        first.SetCondition("seed == 10 && i == 1")
        second = target.BreakpointCreateBySourceRegex(
            SECOND_CONDITION_LINE, lldb.SBFileSpec("main.c")
        )
        second.SetCondition("seed == 20")

        process = target.LaunchSimple(None, None, self.get_process_working_directory())

        # The first condition holds at seed 10, before the second at seed 20.
        self.assertState(process.GetState(), lldb.eStateStopped)
        frame = process.GetSelectedThread().GetFrameAtIndex(0)
        self.assertEqual(frame.FindVariable("seed").GetValueAsSigned(), 10)
        self.assertIn(
            "total += i;", self.get_source_line(frame.GetLineEntry().GetLine())
        )

        process.Continue()
        self.assertState(process.GetState(), lldb.eStateStopped)
        frame = process.GetSelectedThread().GetFrameAtIndex(0)
        self.assertEqual(frame.FindVariable("seed").GetValueAsSigned(), 20)
        self.assertIn(
            "return total;", self.get_source_line(frame.GetLineEntry().GetLine())
        )

        # Neither fell back: a second condition that could not be compiled in
        # would leave the first working and report why, so hit counts alone
        # would not tell the two apart.
        self.assertEqual(first.GetHitCount(), 1)
        self.assertEqual(second.GetHitCount(), 1)
        self.assertLess(
            process.GetStopID(True), STOPS_ALLOWANCE, "the run paid for no hits"
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_removing_one_condition_leaves_the_other(self):
        """Dropping one injection recompiles what remains."""
        target = self.setup()
        first = target.BreakpointCreateBySourceRegex(
            CONDITION_LINE, lldb.SBFileSpec("main.c")
        )
        first.SetCondition("seed == 10 && i == 1")
        second = target.BreakpointCreateBySourceRegex(
            SECOND_CONDITION_LINE, lldb.SBFileSpec("main.c")
        )
        second.SetCondition("seed == 20")
        target.BreakpointDelete(first.GetID())

        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        self.assertState(process.GetState(), lldb.eStateStopped)
        frame = process.GetSelectedThread().GetFrameAtIndex(0)
        self.assertEqual(frame.FindVariable("seed").GetValueAsSigned(), 20)
        self.assertEqual(second.GetHitCount(), 1)

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_a_disabled_breakpoint_does_not_stop(self):
        """Disabling a breakpoint whose condition is compiled in stops the stops.

        The trap in the copy goes on firing wherever the condition holds, because
        the condition is all the copy tests. Everything else the breakpoint says
        about its hits is decided where the hit is reported, exactly as it is for
        a hit reported at the location's own stop -- so a disabled breakpoint
        neither stops nor counts.
        """
        target = self.setup()
        bp = target.BreakpointCreateBySourceRegex(
            CONDITION_LINE, lldb.SBFileSpec("main.c")
        )
        # True on the last hundred calls, so that a hit which wrongly stopped
        # costs a stop that can be counted rather than a wait that cannot.
        bp.SetCondition("seed >= %d && i == 1" % (CALLS - 100))

        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        self.assertState(process.GetState(), lldb.eStateStopped)
        self.assertEqual(bp.GetHitCount(), 1)

        bp.SetEnabled(False)
        process.Continue()
        self.assertState(process.GetState(), lldb.eStateExited)
        self.assertEqual(process.GetExitStatus(), 0)
        self.assertEqual(
            bp.GetHitCount(), 1, "the ninety-nine hits that followed were not its"
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_an_ignore_count_is_honoured(self):
        """An ignore count counts hits the compiled-in condition trapped for."""
        target = self.setup()
        bp = target.BreakpointCreateBySourceRegex(
            CONDITION_LINE, lldb.SBFileSpec("main.c")
        )
        # True once per call, so the hits are one per value of seed from zero.
        bp.SetCondition("i == 1")
        bp.SetIgnoreCount(3)

        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        self.assertState(process.GetState(), lldb.eStateStopped)
        frame = process.GetSelectedThread().GetFrameAtIndex(0)
        self.assertEqual(
            frame.FindVariable("seed").GetValueAsSigned(),
            3,
            "the first three hits were ignored",
        )
        self.assertEqual(bp.GetHitCount(), 4, "and counted")

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_a_one_shot_breakpoint_stops_once(self):
        """A one-shot breakpoint is deleted by the hit it stops for.

        And with it goes the injection: the trap that survived it would take a
        stop of its own on every later hit, silently, which is the cost compiling
        the condition in was for.
        """
        target = self.setup()
        bp = target.BreakpointCreateBySourceRegex(
            CONDITION_LINE, lldb.SBFileSpec("main.c")
        )
        # True on the last hundred calls, so a trap left in the program costs a
        # measurable number of stops rather than an unmeasurable wait.
        bp.SetCondition("seed >= %d && i == 1" % (CALLS - 100))
        bp.SetOneShot(True)

        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        self.assertState(process.GetState(), lldb.eStateStopped)
        self.assertEqual(
            process.GetSelectedThread().GetFrameAtIndex(0)
            .FindVariable("seed")
            .GetValueAsSigned(),
            CALLS - 100,
        )
        self.assertEqual(target.GetNumBreakpoints(), 0, "the breakpoint is gone")

        stops = process.GetStopID(True)
        process.Continue()
        self.assertState(process.GetState(), lldb.eStateExited)
        self.assertEqual(process.GetExitStatus(), 0)
        # One stop is allowed for: the trap is what discovers that the breakpoint
        # it reports to has gone, and taking itself out of the program is what it
        # does about that.
        self.assertLess(
            process.GetStopID(True) - stops, 10, "no trap was left behind firing"
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_deleting_a_breakpoint_takes_its_trap_out_of_the_program(self):
        """A deleted breakpoint stops costing stops.

        The copy stays in the program -- putting a redirected entry back is not
        something that can be done to a running program safely -- but the
        injection reporting to a breakpoint that is gone comes out of it.
        """
        target = self.setup()
        bp = target.BreakpointCreateBySourceRegex(
            CONDITION_LINE, lldb.SBFileSpec("main.c")
        )
        bp.SetCondition("seed >= %d && i == 1" % (CALLS - 100))

        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        self.assertState(process.GetState(), lldb.eStateStopped)
        self.assertEqual(bp.GetHitCount(), 1)

        stops = process.GetStopID(True)
        target.BreakpointDelete(bp.GetID())
        process.Continue()
        self.assertState(process.GetState(), lldb.eStateExited)
        self.assertEqual(process.GetExitStatus(), 0)
        self.assertLess(
            process.GetStopID(True) - stops,
            10,
            "the remaining ninety-nine hits cost no stops",
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_an_edited_condition_takes_effect(self):
        """Editing a condition that is compiled in recompiles the copy.

        A copy tests the text it was compiled with, so an edit that did not reach
        it would leave the program stopping where the replaced text said to --
        and the location's own trap, which is where a condition is ordinarily
        evaluated, sits in a body the redirect no longer reaches.
        """
        target = self.setup()
        bp = target.BreakpointCreateBySourceRegex(
            CONDITION_LINE, lldb.SBFileSpec("main.c")
        )
        bp.SetCondition("seed == 10 && i == 1")

        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        self.assertState(process.GetState(), lldb.eStateStopped)
        frame = process.GetSelectedThread().GetFrameAtIndex(0)
        self.assertEqual(frame.FindVariable("seed").GetValueAsSigned(), 10)

        bp.SetCondition("seed == 20 && i == 1")
        process.Continue()
        self.assertState(process.GetState(), lldb.eStateStopped)
        frame = process.GetSelectedThread().GetFrameAtIndex(0)
        self.assertEqual(frame.FindVariable("seed").GetValueAsSigned(), 20)
        self.assertEqual(bp.GetHitCount(), 2)
        # Still compiled in, rather than having fallen back to a stop per hit.
        self.assertLess(
            process.GetStopID(True), STOPS_ALLOWANCE, "the run paid for no hits"
        )

    @skipUnlessDarwin
    @skipIf(archs=no_match(["arm64", "arm64e", "aarch64"]))
    def test_a_cleared_condition_stops_on_every_hit(self):
        """Clearing a condition that is compiled in makes every hit stop.

        Which is what an unconditional breakpoint is. It cannot be done by taking
        the injection out and leaving it at that: the location's own trap is in
        the body the redirect stopped reaching, so what goes into the copy in
        place of the condition is an injection that traps on every hit.
        """
        target = self.setup()
        bp = target.BreakpointCreateBySourceRegex(
            CONDITION_LINE, lldb.SBFileSpec("main.c")
        )
        bp.SetCondition("seed == 10 && i == 1")

        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        self.assertState(process.GetState(), lldb.eStateStopped)
        frame = process.GetSelectedThread().GetFrameAtIndex(0)
        self.assertEqual(frame.FindVariable("i").GetValueAsSigned(), 1)

        bp.SetCondition(None)
        process.Continue()
        self.assertState(process.GetState(), lldb.eStateStopped)
        frame = process.GetSelectedThread().GetFrameAtIndex(0)
        # The next call rather than the next pass of the same one. Clearing the
        # condition compiles a fresh copy and points the entry at it, and the call
        # in progress goes on running the copy it replaced -- which tests the
        # condition that was cleared and whose traps have been silenced.
        self.assertEqual(frame.FindVariable("seed").GetValueAsSigned(), 11)
        self.assertEqual(frame.FindVariable("i").GetValueAsSigned(), 0)
        self.assertEqual(bp.GetHitCount(), 2)

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
