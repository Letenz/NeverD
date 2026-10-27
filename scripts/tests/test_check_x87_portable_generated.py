"""Strictness tests for generated x87 evidence, independent of running fixtures."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_x87_portable_generated as recorder


def fixture():
    commands, observations = [], []
    for name, shape in recorder.expected_artifacts().items():
        commands.append({"artifact": name})
        for case in sorted(recorder.expected_cases(shape)):
            status = ((2, 6, 5, 9, 10, 12, 13)[int(case[8:])]
                      if case.startswith("refusal-") else
                      1 if case.startswith("null-") else 0)
            observations.append({"artifact": name, "case": case, "status": status,
                                 "before": "00" * 528, "after": "00" * 528,
                                 "native_reference": None, "return": "0"})
    return commands, observations


class PortableGeneratedEvidenceTests(unittest.TestCase):
    def test_complete_protocol_has_fixed_coverage(self):
        commands, observations = fixture()
        result = recorder.audit_observations(commands, observations, False)
        self.assertEqual(result["artifacts"], 36)
        self.assertEqual(result["observations"], 6096)
        self.assertEqual(result["distinct_cases"], 508)
        self.assertEqual(result["statuses"]["0"], 5772)
        self.assertEqual(result["statuses"]["1"], 72)
        self.assertEqual(result["native_x64_original_comparisons"], 0)

    def test_missing_case_or_duplicate_cannot_count_as_coverage(self):
        commands, observations = fixture()
        with self.assertRaisesRegex(ValueError, "missing"):
            recorder.audit_observations(commands, observations[:-1])
        with self.assertRaisesRegex(ValueError, "duplicate"):
            recorder.audit_observations(commands, observations + observations[:1])
        with self.assertRaisesRegex(ValueError, "artifact"):
            recorder.audit_observations(commands + commands[:1], observations)

    def test_a_refusal_cannot_become_silent_success(self):
        commands, observations = fixture()
        row = next(item for item in observations if item["case"] == "refusal-0")
        row["status"] = 0
        with self.assertRaisesRegex(ValueError, "status"):
            recorder.audit_observations(commands, observations)

    def test_cross_route_disagreement_and_partial_state_fail(self):
        commands, observations = fixture()
        observations[-1]["return"] = "1"
        with self.assertRaisesRegex(ValueError, "disagree"):
            recorder.audit_observations(commands, observations)
        observations[-1]["return"] = "0"
        observations[0]["after"] = "00" * 527
        with self.assertRaisesRegex(ValueError, "complete"):
            recorder.audit_observations(commands, observations)

    def test_native_reference_is_host_specific_and_required_on_x64(self):
        commands, observations = fixture()
        with self.assertRaisesRegex(ValueError, "missing native"):
            recorder.audit_observations(commands, observations, True)
        for row in observations:
            if row["case"].startswith("exact-"):
                row["native_reference"] = {"defined": "fixture"}
        result = recorder.audit_observations(commands, observations, True)
        self.assertEqual(result["native_x64_original_comparisons"], 5760)
        with self.assertRaisesRegex(ValueError, "non-x64"):
            recorder.audit_observations(commands, observations, False)

    def test_skip_or_missing_named_test_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.xml"
            path.write_text('<testsuites><testsuite><testcase classname="X87PortableGenerated" '
                            'name="AllRoutesExecuteExactStateAndRefusalBoundaries" '
                            'status="run" result="completed"/></testsuite></testsuites>')
            recorder.audit_tests(path)
            path.write_text(path.read_text().replace('/>', '><skipped message="unavailable"/></testcase>'))
            with self.assertRaisesRegex(ValueError, "without skips"):
                recorder.audit_tests(path)
            path.write_text('<testsuites/>')
            with self.assertRaisesRegex(ValueError, "exactly"):
                recorder.audit_tests(path)


if __name__ == "__main__":
    unittest.main()
