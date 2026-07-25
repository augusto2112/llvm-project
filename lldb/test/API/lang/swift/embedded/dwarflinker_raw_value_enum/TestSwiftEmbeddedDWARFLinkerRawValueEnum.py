"""
Test that dsymutil preserves the DW_TAG_enumerator children of an embedded
Swift raw-value enum.

Embedded Swift emits no reflection metadata, so a raw-value enum whose
enumerators were dropped by the DWARFLinker leaves LLDB with no way to name a
case: the enumeration type still has a DW_AT_byte_size, so it looks complete,
but every case name is gone.

This only reproduces in the dsym debug-info variant, since the truncation
happens inside dsymutil. In the dwarf variant LLDB reads the unlinked object
file, which always has the enumerators, so that variant passes either way.
"""

import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil


class TestSwiftEmbeddedDWARFLinkerRawValueEnum(TestBase):
    @skipUnlessDarwin
    @swiftTest
    @skipUnlessEmbeddedSwift
    def test(self):
        """A raw-value enum keeps its case names after dsymutil links the dSYM."""
        self.build()
        self.runCmd("setting set symbols.swift-enable-ast-context false")

        lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )

        # Without the DWARFLinker fix the dSYM's Event has zero
        # DW_TAG_enumerator children, and TypeSystemSwiftTypeRef cannot build a
        # case list for it, so this prints "unimplemented enum kind" instead of
        # the case name.
        self.expect(
            "frame variable ev",
            substrs=["Event", "fault"],
            error=False,
            matching=True,
        )
        self.expect(
            "frame variable ev",
            substrs=["unimplemented enum kind"],
            matching=False,
        )
