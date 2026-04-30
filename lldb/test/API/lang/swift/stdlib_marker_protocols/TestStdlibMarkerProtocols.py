import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil


class TestStdlibMarkerProtocols(TestBase):
    @swiftTest
    def test(self):
        self.build()
        self.runCmd("settings set symbols.swift-enable-ast-context false")
        lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )

        self.expect("frame variable sendable", substrs=["1"])
        self.expect("frame variable bitwiseCopyable", substrs=["2"])
        self.expect("frame variable sendableAndBitwise", substrs=["3"])
        self.expect("frame variable realAndSendable", substrs=["4"])
