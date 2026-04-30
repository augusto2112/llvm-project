import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil


class TestNestedMarkerProtocolExistential(TestBase):
    @swiftTest
    def test(self):
        self.build()
        self.runCmd("settings set symbols.swift-enable-ast-context false")
        lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )

        self.expect("frame variable bareMarker", substrs=["1"])
        self.expect("frame variable bareComposition", substrs=["bar = 2"])
        self.expect(
            "frame variable arrayOfMarker", substrs=["5", "6", "7"]
        )
        self.expect(
            "frame variable genericBox", substrs=["bar = 8", "tag = 9"]
        )
