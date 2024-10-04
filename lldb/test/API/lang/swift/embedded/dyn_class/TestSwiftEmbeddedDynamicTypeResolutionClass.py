import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil


class TestSwiftEmbeddedDynamicTypeResolutionClass(TestBase):
    @skipUnlessDarwin
    @swiftTest
    def test(self):
        self.build()

        lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )
        self.expect('frame variable b', substrs=['a.B', 'i = 42', 'j = 4329'])

        self.expect('frame variable c', substrs=['a.C', 'i = 42', 'j = 4329', 'k = 98'])
