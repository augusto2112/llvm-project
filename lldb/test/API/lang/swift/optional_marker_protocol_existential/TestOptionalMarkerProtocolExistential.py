import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil


class TestOptionalMarkerProtocolExistential(TestBase):
    @swiftTest
    @expectedFailureAll
    def test(self):
        self.build()
        self.runCmd("settings set symbols.swift-enable-ast-context false")
        lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )
        self.expect("frame variable optMarker", substrs=["bar = 3"])
