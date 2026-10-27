"""Evidence must contain actual native tests, every control and every object."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_x87_translation as recorder


def fixture():
    rows = []
    for top in range(8):
        for pc in (0, 2, 3):
            for rc in range(4):
                for level in (0, 2):
                    case = f"exact-{top}-{pc}-{rc}"
                    rows.append({"case": case, "level": level, "exit_reason": 1,
                        "guest_instructions": 7, "blocks_translated": 6,
                        "generated_bytes": 1, "before": "00", "after": "01",
                        "session_after": "01", "blocks": [
                            {"object": f"{case}-O{level}-block-{i}.o", "guest_bytes": code,
                             "status": 260 if i == 5 else 256, "audit_stages": 511,
                             "registry": "registry", "artifact_identity": "object",
                             "attempted": i != 5, "completed": i != 5,
                             "portable_status": 1 if i == 5 else 0, "after": "01"}
                            for i, code in enumerate(["48C7C000000000DB2F", "DB2E", "DEC1",
                                                       "DB3A", "DFE0", "C3"])]})
    return rows


class X87TranslationEvidenceTests(unittest.TestCase):
    def test_complete_control_and_object_counts(self):
        coverage, objects = recorder.audit_observations(fixture())
        self.assertEqual(coverage["cases"], 96)
        self.assertEqual(coverage["observations"], 192)
        self.assertEqual(len(objects), 1152)

    def test_missing_duplicate_and_changed_identity_fail(self):
        rows = fixture()
        for bad in (rows[:-1], rows + rows[:1]):
            with self.assertRaises(ValueError):
                recorder.audit_observations(bad)
        rows[0]["blocks"][0]["object"] = "../other.o"
        with self.assertRaisesRegex(ValueError, "object"):
            recorder.audit_observations(rows)

    def test_terminal_protocol_and_audit_cannot_be_omitted(self):
        for field, value in (("status", 0), ("audit_stages", 255), ("completed", False),
                             ("portable_status", 13), ("guest_bytes", "90")):
            rows = fixture()
            rows[0]["blocks"][0][field] = value
            with self.assertRaisesRegex(ValueError, "protocol"):
                recorder.audit_observations(rows)

    def test_state_and_optimization_disagreement_fail(self):
        rows = fixture()
        rows[0]["session_after"] = "02"
        with self.assertRaisesRegex(ValueError, "disagree"):
            recorder.audit_observations(rows)
        rows = fixture()
        rows[1]["blocks"][0]["after"] = "02"
        with self.assertRaisesRegex(ValueError, "disagree"):
            recorder.audit_observations(rows)

    def test_skipped_missing_failed_and_duplicate_native_tests_fail(self):
        cases = ['<testcase classname="' + name.rsplit('.', 1)[0] + '" name="' +
                 name.rsplit('.', 1)[1] + '" status="run" result="completed"/>'
                 for name in sorted(recorder.EXPECTED_TESTS)]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.xml"
            path.write_text('<testsuites>' + ''.join(cases) + '</testsuites>')
            self.assertEqual(recorder.audit_tests(path), 13)
            for bad in (cases[:-1], cases + cases[:1],
                        [cases[0].replace('/>', '><skipped/></testcase>')] + cases[1:],
                        [cases[0].replace('/>', '><failure/></testcase>')] + cases[1:]):
                path.write_text('<testsuites>' + ''.join(bad) + '</testsuites>')
                with self.assertRaises(ValueError):
                    recorder.audit_tests(path)


if __name__ == "__main__":
    unittest.main()
