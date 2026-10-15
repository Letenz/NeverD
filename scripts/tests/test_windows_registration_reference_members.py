import copy
from pathlib import Path
import tempfile
import unittest

from scripts import check_windows_registration_nested_try as runner
from scripts import replay_windows_registration_nested_try as replay
from scripts import windows_registration_reference_members as members
from scripts import windows_registration_reference_members_relift as relift
from scripts.tests import test_windows_registration_destructors as fixtures


class ReferenceMemberEvidenceTests(unittest.TestCase):
    def capture(self, root):
        capture = fixtures.DestructorEvidenceTests().capture(root)
        profile = members.profile(runner.validate_decompilation)
        capture.update(profile="reference-members",
                       source_sha256=runner.file_digest(profile.source),
                       object_types_sha256=runner.file_digest(profile.types),
                       object_proof_sha256=runner.file_digest(profile.proof),
                       source_dependencies_sha256={
                           path.name: runner.file_digest(path) for path in profile.dependencies})
        return capture

    def test_member_evidence_requires_current_source_and_receiver_proof(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            self.assertEqual(len(replay.validate_capture(root, capture)), 128)
            self.assertEqual(len(relift.validate_first(root, capture)), 128)
            for key in ("source_sha256", "object_types_sha256", "object_proof_sha256"):
                changed = copy.deepcopy(capture)
                changed[key] = "stale"
                with self.subTest(key=key), self.assertRaises(ValueError):
                    replay.validate_capture(root, changed)
            for dependency in capture["source_dependencies_sha256"]:
                changed = copy.deepcopy(capture)
                del changed["source_dependencies_sha256"][dependency]
                with self.subTest(dependency=dependency), self.assertRaises(ValueError):
                    relift.validate_first(root, changed)
            for name in (None, "destructor-results", "destructor-graphs", "destructors"):
                changed = copy.deepcopy(capture)
                changed["profile"] = name
                with self.subTest(profile=name), self.assertRaises(ValueError):
                    relift.validate_first(root, changed)

    def test_member_matrix_requires_both_optimizations_and_negative_controls(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            self.assertEqual(len(capture["cases"]), 16)
            for index in range(len(capture["cases"])):
                changed = copy.deepcopy(capture)
                del changed["cases"][index]
                with self.subTest(index=index), self.assertRaises(ValueError):
                    replay.validate_capture(root, changed)
            (root / capture["cases"][0]["case"] / "rewrite.xml").write_text(
                '<testsuites tests="2"><testcase/><testcase><skipped/></testcase></testsuites>')
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)


if __name__ == "__main__":
    unittest.main()
