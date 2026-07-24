"""
End-to-end check for the reflection-vs-DWARF differential validation report
mode: with a journal directory set, divergences are recorded (not asserted) and
the run completes, producing valid schema-1 JSONL records.
"""
import glob
import json
import os

import lldb
import lldbsuite.test.lldbutil as lldbutil
from lldbsuite.test.decorators import skipEmbeddedSwift, swiftTest
from lldbsuite.test.lldbtest import TestBase


class TestDwarfValidationJournal(TestBase):
    NO_DEBUG_INFO_TESTCASE = True

    # Embedded Swift emits no reflection metadata, so the reflection-vs-DWARF
    # differential (which compares a reflection-only against a DWARF-only shadow
    # context) does not apply there. `print`'s embedded overloads also reject
    # this source. Restrict to the regular-Swift variant.
    @skipEmbeddedSwift
    @swiftTest
    def test_report_mode_writes_valid_records(self):
        journal_dir = os.path.join(self.getBuildDir(), "journal")
        lldbutil.mkdir_p(journal_dir)

        # Provenance: the C++ reads this env via getenv and copies it into each
        # record's "test" field.
        os.environ["LLDB_SWIFT_VALIDATE_DWARF_TEST_BUILDDIR"] = self.getBuildDir()

        # Enable report mode at the strictest level. These must be set before
        # the reflection context is created (i.e. before the first type query).
        self.runCmd("settings set symbols.swift-validate-typesystem true")
        self.runCmd("settings set symbols.swift-validate-typesystem-dwarf strict")
        self.runCmd(
            "settings set -- symbols.swift-validate-typesystem-dwarf-journal %s"
            % journal_dir
        )

        self.build()
        lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )

        # Exercise type-info queries on the class/struct so the differential
        # comparison runs. If report mode were broken (asserting), the process
        # would abort here and the test would fail.
        self.runCmd("frame variable foo bar", check=False)
        self.runCmd("expression -- foo.x", check=False)
        self.runCmd("expression -- bar", check=False)

        files = glob.glob(os.path.join(journal_dir, "divergences-*.jsonl"))
        self.assertTrue(
            len(files) > 0,
            "expected at least one per-process journal file in %s" % journal_dir,
        )

        records = []
        for path in files:
            with open(path) as fh:
                for line in fh:
                    line = line.strip()
                    if not line:
                        continue
                    rec = json.loads(line)  # Asserts each line is valid JSON.
                    self.assertEqual(rec["schema"], 1)
                    self.assertIn(rec["kind"], ("divergence", "asymmetry"))
                    self.assertIn("type_mangled", rec)
                    self.assertIn("root_cause_key", rec)
                    records.append(rec)

        # `strict` on a class reliably surfaces divergences DWARF cannot express
        # (e.g. num_extra_inhabitants / borrowability), so at least one record
        # must exist. If a future toolchain genuinely produces zero, relax this
        # to assert only validity of any records found.
        self.assertTrue(
            len(records) > 0,
            "expected >=1 divergence/asymmetry record at strict level",
        )
