import contextlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

from scripts import run_native_cpu_ci as native
from scripts.audit_ci_test_inventory import TestRecord
from scripts.tests.test_audit_ci_test_results import junit


class NativeCPUEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "build"
        self.evidence = self.root / "evidence"
        (self.root / "scripts").mkdir()
        (self.root / "scripts" / "NativeCPUTests.def").write_text(
            'NEVERD_NATIVE_CPU_OWNER(Owner)\n'
            'NEVERD_NATIVE_CPU_REQUIRED_CASES("Native/Case/", "cases.def", "CASE")\n'
        )
        (self.root / "cases.def").write_text("CASE(First, 1)\nCASE(Second, 2)\n")
        (self.build / "CMakeFiles").mkdir(parents=True)
        (self.build / "CMakeFiles" / "TargetDirectories.txt").write_text(
            str(self.build / "unittests/emulation/CMakeFiles/Owner.dir") + "\n"
        )
        self.records = tuple(
            TestRecord(name, frozenset({"Owner"}))
            for name in ("Native/Case/First", "Native/Case/Second", "Portable/Case")
        )
        self.reported = self.records
        self.changes = {}
        self.status = 0
        self.executions = []

    def execute(self, command, **kwargs):
        self.executions.append(command)
        if command[0] == "ctest":
            document = junit(self.reported, self.changes)
            ET.ElementTree(document).write(self.evidence / "results.xml")
        return subprocess.CompletedProcess(command, self.status)

    def capture(self, command, **kwargs):
        if command[0] == "git":
            return "test-commit\n"
        return json.dumps({
            "kind": "ctestInfo", "version": {"major": 1},
            "tests": [
                {"name": record.name, "properties": [
                    {"name": "LABELS", "value": sorted(record.labels)}
                ]}
                for record in self.records
            ],
        })

    def run_evidence(self):
        with (
            mock.patch.object(native, "ROOT", self.root),
            mock.patch.object(native.subprocess, "run", side_effect=self.execute),
            mock.patch.object(
                native.subprocess, "check_output", side_effect=self.capture
            ),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            return native.run(self.build, self.evidence, 2, True)

    def summary(self):
        return json.loads((self.evidence / "summary.json").read_text())

    def test_all_required_native_cases_pass_while_optional_software_skips(self):
        self.changes[self.records[-1]] = (
            "notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", "software disabled"
        )
        self.assertEqual(self.run_evidence(), 0)
        self.assertEqual(self.summary()["counts"]["passed"], 2)
        self.assertEqual(self.summary()["counts"]["skipped"], 1)
        self.assertIn("Owner", self.executions[0])

    def test_deleted_native_registration_cannot_pass_vacuously(self):
        self.records = self.records[1:]
        with self.assertRaisesRegex(ValueError, "missing required native WHP"):
            self.run_evidence()
        self.assertEqual(len(self.executions), 1)

    def test_native_skip_is_failure_even_when_ctest_returns_zero(self):
        self.changes[self.records[0]] = (
            "notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", "host unavailable"
        )
        self.assertEqual(self.run_evidence(), 1)
        self.assertEqual(
            self.summary()["required_native_unexecuted"], [self.records[0].name]
        )

    def test_infrastructure_skip_remains_not_run(self):
        self.changes[self.records[-1]] = ("notrun", "Unable to find executable", "")
        self.assertEqual(self.run_evidence(), 1)
        self.assertEqual(self.summary()["counts"]["not_run"], 1)
        self.assertEqual(self.summary()["counts"]["skipped"], 0)

    def test_same_count_with_different_owner_identity_is_failure(self):
        self.reported = (
            *self.records[:-1],
            TestRecord("Portable/Case", frozenset({"Other"})),
        )
        self.assertEqual(self.run_evidence(), 1)
        self.assertEqual(self.summary()["missing"], ["Portable/Case"])
        self.assertEqual(self.summary()["unexpected"], ["Portable/Case"])

    def test_missing_outcome_cannot_be_hidden_by_successful_exit(self):
        self.reported = self.records[:-1]
        self.assertEqual(self.run_evidence(), 1)
        self.assertEqual(self.summary()["missing"], [self.records[-1].name])

    def test_failed_or_disabled_case_prevents_success(self):
        for state in ("fail", "disabled"):
            with self.subTest(state=state):
                self.changes[self.records[-1]] = (state, "", "")
                self.assertEqual(self.run_evidence(), 1)

    def test_missing_owner_fails_before_build(self):
        (self.build / "CMakeFiles" / "TargetDirectories.txt").write_text("")
        with self.assertRaisesRegex(ValueError, "unconfigured"):
            self.run_evidence()
        self.assertEqual(self.executions, [])

    def test_declared_cases_follow_the_independent_def_inventory(self):
        owners, required = native.declared_inventory(self.root)
        self.assertEqual(owners, ["Owner"])
        self.assertEqual(required, {"Native/Case/First", "Native/Case/Second"})
        (self.root / "cases.def").write_text("CASE(First, 1)\nCASE(Third, 3)\n")
        _, required = native.declared_inventory(self.root)
        self.assertEqual(required, {"Native/Case/First", "Native/Case/Third"})


if __name__ == "__main__":
    unittest.main()
