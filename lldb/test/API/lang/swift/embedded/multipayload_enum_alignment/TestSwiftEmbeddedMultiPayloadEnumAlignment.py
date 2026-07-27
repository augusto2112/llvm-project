"""
Test that a multi-payload enum's alignment is not fabricated from its
DW_AT_byte_size in embedded Swift.

Embedded Swift emits no reflection metadata, so DWARF is LLDB's only source of
type information and every type goes through the DWARF DescriptorFinder. The
compiler does not emit DW_AT_alignment on a multi-payload enum's composite DIE
(IRGenDebugInfo passes an alignment of zero for a default-aligned type and LLVM
then omits the attribute). getDWARFBuiltinTypeDescriptor used to default the
alignment to DW_AT_byte_size and round the stride up to it, so an enum with
byte_size 34 got alignment 34 and stride 66 instead of alignment 8, stride 40.

The size of the enum itself comes straight from DW_AT_byte_size and is right
either way; the fabricated alignment only shows up in the *stride*, so the
assertions here read the stride out of aggregates that embed the enum.
"""

import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil


class TestSwiftEmbeddedMultiPayloadEnumAlignment(TestBase):
    def setup_test(self):
        self.build()
        self.runCmd("setting set symbols.swift-enable-ast-context false")
        _, _, thread, _ = lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )
        return thread.GetSelectedFrame()

    @skipUnlessDarwin
    @swiftTest
    @skipUnlessEmbeddedSwift
    def test_enum_stride(self):
        """
        The enum's size is DW_AT_byte_size = 34. Its stride is
        alignUp(34, 8) = 40, where 8 is the maximum of the payload alignments.
        Two enums back to back put the second one at that stride, so EnumPair is
        40 + 34 = 74 bytes. With the alignment fabricated from DW_AT_byte_size
        the stride was alignUp(34, 34) = 66 and EnumPair was 100 bytes.
        """
        frame = self.setup_test()

        wide_enum = frame.FindVariable("w").GetType()
        self.assertIn("WideEnum", wide_enum.GetName())
        self.assertEqual(wide_enum.GetByteSize(), 34)

        pairs = frame.FindVariable("pairs")
        self.assertTrue(pairs.IsValid(), "pairs is valid")
        self.assertEqual(pairs.GetType().GetByteSize(), 74)
        self.assertEqual(pairs.GetNumChildren(), 2)

        base = pairs.GetLoadAddress()
        self.assertNotEqual(base, lldb.LLDB_INVALID_ADDRESS)
        self.assertEqual(pairs.GetChildAtIndex(0).GetLoadAddress() - base, 0)
        self.assertEqual(
            pairs.GetChildAtIndex(1).GetLoadAddress() - base,
            40,
            "WideEnum's stride is alignUp(34, 8) = 40, not alignUp(34, 34) = 66",
        )

        # And the values read out of those offsets have to be right, which they
        # are not if the second field is looked up 66 bytes in.
        self.expect(
            "frame variable pairs",
            substrs=["first", "pair", "0 = 7", "1 = 8", "second", "0 = 9", "1 = 10"],
        )

    @skipUnlessDarwin
    @swiftTest
    @skipUnlessEmbeddedSwift
    def test_enum_alignment(self):
        """
        A one-byte field followed by the enum: the enum lands at
        alignUp(1, alignment). The correct alignment 8 puts it at offset 8 and
        makes the struct 8 + 34 = 42 bytes. With the alignment fabricated as 34
        the mask-based round-up ((1 + 33) & ~33) yielded 2, putting the payload
        at offset 2 and making the struct 36 bytes.
        """
        frame = self.setup_test()

        prefixed = frame.FindVariable("prefixed")
        self.assertTrue(prefixed.IsValid(), "prefixed is valid")
        self.assertEqual(prefixed.GetType().GetByteSize(), 42)

        base = prefixed.GetLoadAddress()
        self.assertNotEqual(base, lldb.LLDB_INVALID_ADDRESS)
        payload = prefixed.GetChildMemberWithName("payload")
        self.assertTrue(payload.IsValid(), "payload is valid")
        self.assertEqual(
            payload.GetLoadAddress() - base,
            8,
            "WideEnum's alignment is 8 (the max of the payload alignments), not 34",
        )

        self.expect(
            "frame variable prefixed",
            substrs=["tag = 3", "payload", "pair", "0 = 11", "1 = 12"],
        )
