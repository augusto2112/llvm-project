
"""
Test that a C++ class is visible in Swift.
"""
import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil
import os
import unittest2


class TestClass(TestBase):

    @swiftTest
    def test_class(self):
        self.build()
        self.runCmd('setting set target.experimental.swift-enable-cxx-interop true')
        target, process, thread, bkpt = lldbutil.run_to_source_breakpoint(
            self, 'Set breakpoint here', lldb.SBFileSpec('main.swift'))

        self.expect('v x', substrs=['CxxStruct', 'a1', '1', 'a2', '2', 'a3', '3'])
        self.expect('po x', substrs=['CxxStruct', 'a1', '1', 'a2', '2', 'a3', '3'])
