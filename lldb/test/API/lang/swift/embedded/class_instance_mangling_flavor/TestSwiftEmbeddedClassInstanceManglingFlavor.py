"""
Regression test for re-mangling class-instance TypeRefs with the caller's
mangling flavor.

`TypeRef::mangle()` defaults to the `$s` (non-embedded) mangling flavor, and
`CompareShadowsForClassInstance` used to call it without one. A TypeRef stores
its nominal names with the mangling prefix already stripped, so the flavor
cannot be recovered from the TypeRef and has to be threaded in from the caller.

The flavor is consumed in exactly one place: the reflection-vs-DWARF
differential's class-instance comparison. Getting it wrong there is not
cosmetic, because the differential's Embedded Swift opt-out
(`IsEmbeddedSwiftName`) keys off the *re-mangled* name. With the default `$s`
flavor, every Embedded Swift class instance was renamed to a `$s` type that
does not exist in the program, the embedded opt-out therefore did not
recognize it, and the differential went on to compare two shadows keyed by
that nonexistent name.

So the assertion here is: with the differential forced on for an Embedded
Swift program, the class-instance path must produce no journal records at
all -- every embedded class instance must be recognized as embedded and
skipped. Before the fix, the class-instance path leaked `$s`-mangled records
for `$e` types.

Note this test deliberately re-enables the differential, which the swiftembed
variant setup clears (see `_embedded_swift_setup`). That is the point: the
code under test only runs when the differential is active, and the fix is
what makes the embedded opt-out fire once it is.
"""
import glob
import json
import os

import lldb
import lldbsuite.test.lldbutil as lldbutil
from lldbsuite.test.decorators import skipUnlessDarwin, skipUnlessEmbeddedSwift, swiftTest
from lldbsuite.test.lldbtest import TestBase


class TestSwiftEmbeddedClassInstanceManglingFlavor(TestBase):
    @skipUnlessDarwin
    @swiftTest
    @skipUnlessEmbeddedSwift
    def test_class_instance_typerefs_remangle_as_embedded(self):
        journal_dir = os.path.join(self.getBuildDir(), "journal")
        lldbutil.mkdir_p(journal_dir)

        # `IsDwarfValidationActive()` is read in the TargetReflectionContext
        # constructor, so the differential must be switched on before the
        # target exists. Report mode keeps the run going and records what the
        # differential saw instead of asserting on the first divergence.
        self.runCmd("settings set symbols.swift-validate-typesystem true")
        self.runCmd("settings set symbols.swift-validate-typesystem-dwarf strict")
        self.runCmd(
            "settings set -- symbols.swift-validate-typesystem-dwarf-journal %s"
            % journal_dir
        )
        self.runCmd("settings set symbols.swift-enable-ast-context false")

        # Positive control against a vacuous pass. "No journal records" is only
        # meaningful if the differential is actually armed. The swiftembed
        # variant setup (`_embedded_swift_setup`) clears this setting, so if
        # that ever moved to run *after* the test body the assertions below
        # would pass for the wrong reason.
        self.expect(
            "settings show symbols.swift-validate-typesystem-dwarf",
            substrs=["strict"],
        )

        self.build()
        target, process, thread, _ = lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )

        # Drive dynamic type resolution so the class-instance TypeRef path
        # runs: a subclass in a superclass-typed variable, a subclass in a
        # class-constrained existential, and a two-level-deep subclass so the
        # superclass traversal reaches the class-instance path repeatedly.
        self.expect(
            "frame variable asBase",
            substrs=["a.Derived", "a.Base", "baseField = 42", "derivedField = 100"],
        )
        self.expect(
            "frame variable asExistential",
            substrs=["a.Derived", "a.Base", "baseField = 42", "derivedField = 100"],
        )
        self.expect(
            "frame variable deep",
            substrs=["a.Leaf", "a.Derived", "a.Base", "leafField = 7"],
        )

        records = []
        for path in glob.glob(os.path.join(journal_dir, "divergences-*.jsonl")):
            with open(path) as fh:
                for line in fh:
                    line = line.strip()
                    if line:
                        records.append(json.loads(line))

        # Every type in an Embedded Swift program is `$e`-mangled, so the
        # differential's embedded opt-out must swallow all of them. A record
        # naming a `$s` type is the bug: a TypeRef re-mangled with the default
        # flavor, renaming an embedded type to one the program does not define.
        bad = [r for r in records if r.get("type_mangled", "").startswith("$s1a")]
        self.assertEqual(
            bad,
            [],
            "class-instance TypeRefs were re-mangled with the default ($s) "
            "flavor, defeating the differential's Embedded Swift opt-out: %s"
            % [r.get("type_mangled") for r in bad],
        )

        # Specifically, the class-instance producer must contribute nothing.
        from_class_instance = [
            r for r in records if r.get("producer") == "GetClassInstanceTypeInfo"
        ]
        self.assertEqual(
            from_class_instance,
            [],
            "the class-instance path must skip Embedded Swift types entirely, "
            "got: %s" % [r.get("type_mangled") for r in from_class_instance],
        )
