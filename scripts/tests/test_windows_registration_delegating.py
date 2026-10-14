import copy
from pathlib import Path
import tempfile
import unittest

from scripts import check_windows_registration_nested_try as runner
from scripts import replay_windows_registration_nested_try as replay
from scripts import windows_registration_delegating as delegating
from scripts import windows_registration_delegating_relift as relift
from scripts.tests import test_windows_registration_initializers as fixtures


class DelegatingEvidenceTests(unittest.TestCase):
    def capture(self, root):
        capture = fixtures.InitializerEvidenceTests().capture(root)
        profile = delegating.profile(runner.validate_decompilation)
        capture.update(profile="delegating",
                       source_sha256=runner.file_digest(profile.source),
                       object_proof_sha256=runner.file_digest(profile.proof),
                       source_dependencies_sha256={
                           path.name: runner.file_digest(path) for path in profile.dependencies})
        return capture

    def test_transitive_construction_requires_its_own_source_and_proof(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            self.assertEqual(len(replay.validate_capture(root, capture)), 160)
            self.assertEqual(len(relift.validate_first(root, capture)), 160)
            for key in ("source_sha256", "object_proof_sha256", "object_types_sha256"):
                changed = copy.deepcopy(capture)
                changed[key] = "stale"
                with self.subTest(key=key), self.assertRaises(ValueError):
                    replay.validate_capture(root, changed)
            for dependency in capture["source_dependencies_sha256"]:
                changed = copy.deepcopy(capture)
                del changed["source_dependencies_sha256"][dependency]
                with self.subTest(dependency=dependency), self.assertRaises(ValueError):
                    relift.validate_first(root, changed)
            for profile in (None, "initializers", "copies", "objects", "nested"):
                changed = copy.deepcopy(capture)
                changed["profile"] = profile
                with self.subTest(profile=profile), self.assertRaises(ValueError):
                    relift.validate_first(root, changed)

    def test_all_constructor_contexts_optimizations_and_controls_are_required(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            self.assertEqual(len(capture["cases"]), 20)
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
