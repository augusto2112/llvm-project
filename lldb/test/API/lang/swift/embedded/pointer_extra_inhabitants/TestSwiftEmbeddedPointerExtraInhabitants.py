import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil


class TestSwiftEmbeddedPointerExtraInhabitants(TestBase):
    """
    Tests the extra inhabitant count LLDB reports for a not-nullable
    pointer-sized builtin in embedded Swift.

    Embedded Swift emits no reflection metadata, so DWARF is the only source of
    Swift type information and the builtin descriptors come from
    DWARFASTParserSwift::getBuiltinTypeDescriptor. A thin function pointer
    ("yyXf") has no usable DWARF descriptor -- the compiler emits it as a
    DW_TAG_structure_type wrapping a "ptr" member rather than a base type
    carrying DW_AT_LLVM_num_extra_inhabitants -- so it falls back to
    getHardcodedBuiltinTypeDescriptor, which used to answer 1. That is the
    Builtin.RawPointer answer: correct for a nullable raw pointer, whose only
    extra inhabitant is null, but wrong for a function pointer, which is not
    nullable and so has every address below LeastValidPointerValue available.

    The count is observable through nested Optionals, because the Nth level of
    nesting needs an Nth extra inhabitant of the payload. With enough of them
    the whole nest stays pointer sized; with only one, every level past the
    first has to grow an out-of-line discriminator byte.
    """

    @skipUnlessDarwin
    @swiftTest
    @skipUnlessEmbeddedSwift
    def test_not_nullable_pointer_extra_inhabitants(self):
        self.build()
        self.runCmd("setting set symbols.swift-enable-ast-context false")

        target, process, thread, _ = lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )

        frame = thread.GetSelectedFrame()

        # A thin function pointer has enough extra inhabitants that three
        # levels of Optional are still free: every level is spare-bit encoded
        # into the pointer itself, so all three fields are 8 bytes and the
        # struct is 3 * 8 = 24. Reporting a single extra inhabitant instead
        # forces an out-of-line discriminator byte from the second level on,
        # which is exactly the RawHolder layout checked below.
        fn_holder = frame.FindVariable("fnHolder")
        self.assertSuccess(fn_holder.GetError(), "fnHolder is available")
        self.assertEqual(fn_holder.GetType().GetByteSize(), 24)
        for name in ["a", "b", "c"]:
            field = fn_holder.GetChildMemberWithName(name)
            self.assertSuccess(field.GetError(), "fnHolder.%s is available" % name)
            self.assertEqual(
                field.GetType().GetByteSize(),
                8,
                "nested Optional of a thin function pointer stays pointer sized",
            )

        # The control. Builtin.RawPointer is nullable and keeps its count of 1
        # both before and after the fix, so this is what a one-extra-inhabitant
        # pointer-sized builtin looks like: 8, then a byte per extra level.
        # 8 + 9 + (10 padded up to alignment 8, i.e. 16) = 34 with the last
        # field starting at offset 24.
        raw_holder = frame.FindVariable("rawHolder")
        self.assertSuccess(raw_holder.GetError(), "rawHolder is available")
        self.assertEqual(raw_holder.GetType().GetByteSize(), 34)
        for name, size in [("a", 8), ("b", 9), ("c", 10)]:
            field = raw_holder.GetChildMemberWithName(name)
            self.assertSuccess(field.GetError(), "rawHolder.%s is available" % name)
            self.assertEqual(field.GetType().GetByteSize(), size)

        # The two layouts must not agree. If the thin function pointer is ever
        # given RawPointer's extra inhabitant count again, FnHolder collapses
        # onto RawHolder's layout and this is what notices.
        self.assertNotEqual(
            fn_holder.GetType().GetByteSize(),
            raw_holder.GetType().GetByteSize(),
            "a not-nullable function pointer must not be laid out like a "
            "nullable raw pointer",
        )

        # The same layouts as reported through the command interface, so a
        # regression is visible without reading SBType sizes.
        self.expect(
            "frame variable fnHolder",
            substrs=["FnHolder", "a = ", "b = ", "c = "],
        )
