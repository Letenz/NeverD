import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from scripts import check_windows_registration_multiple_catch as catches
from scripts import check_windows_registration_nested_try as runner
from scripts import replay_windows_registration_nested_try as replay
from scripts import windows_registration_destructors as destructors
from scripts import windows_registration_destructors_relift as relift
from scripts.tests import test_windows_registration_objects as fixtures


class DestructorEvidenceTests(unittest.TestCase):
    def capture(self, root):
        capture = fixtures.ObjectEvidenceTests().capture(root)
        capture.update(profile="destructors",
                       source_sha256=runner.file_digest(destructors.SOURCE),
                       object_types_sha256=runner.file_digest(destructors.TYPES),
                       object_proof_sha256=runner.file_digest(destructors.PROOF),
                       source_dependencies_sha256={
                           destructors.SHARED_DRIVER.name:
                               runner.file_digest(destructors.SHARED_DRIVER)})
        return capture

    def test_lifetime_matrix_binds_its_source_dependencies_and_proof(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            self.assertEqual(len(replay.validate_capture(root, capture)), 128)
            self.assertEqual(len(relift.validate_first(root, capture)), 128)
            for field, value in (("profile", "objects"), ("profile", "nested"),
                                 ("object_proof_sha256", "stale"),
                                 ("object_types_sha256", "stale"),
                                 ("source_dependencies_sha256", {}),
                                 ("source_dependencies_sha256", {
                                     destructors.SHARED_DRIVER.name: "stale"})):
                changed = copy.deepcopy(capture)
                changed[field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    replay.validate_capture(root, changed)
            for profile in (None, "objects", "nested"):
                changed = copy.deepcopy(capture)
                changed["profile"] = profile
                with self.subTest(profile=profile), self.assertRaises(ValueError):
                    relift.validate_first(root, changed)
            (root / capture["cases"][0]["case"] / "rewrite.xml").write_text(
                '<testsuites tests="2"><testcase/><testcase><skipped/></testcase></testsuites>')
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)

    def test_lifetime_observation_requires_count_order_contents_and_control(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.capture(root)
            # These constants describe actual catch-copy and exception-object
            # values, independently of the runtime's reported trace.
            traces = {"nested": 0x8810D940, "rethrow": 0xD50DA380}
            for family, trace in traces.items():
                case = family + "-o0-llvm-fixed"
                path = root / case / "original.exe"
                receipt = json.loads((path.parent / "contract.json").read_text())
                values = ([39, 28, 39, 39, 18, 39, 1, 12] if family == "rethrow"
                          else [17, 28, 39, 7, 18, 39, 1, 12])
                values += [0x401010, 0x401020, 0x401030]
                prefix = "MULTICATCH " + " ".join(f"{v:08X}" for v in values) + "\n"
                lifetime = f"LIFETIME 0000000C {trace:08X}\n"
                self.assertEqual(destructors.expected_lifetime(case), (12, trace))
                for control in (False, True):
                    name = case + ("-control" if control else "")
                    result = {"exit_code": int(control), "stdout": prefix + lifetime}
                    with patch.object(catches, "run_image", return_value=result):
                        destructors.observe(path, name, "original", receipt, [], {}, 1)
                        relift.observe(path, name, "original", receipt, [], {}, 1,
                                       source_section=".text")
                    for changed in ("", lifetime.replace("0000000C", "0000000B"),
                                    lifetime.replace(f"{trace:08X}", "00000000"),
                                    lifetime + lifetime, lifetime + "extra\n"):
                        result["stdout"] = prefix + changed
                        with self.subTest(family=family, control=control, output=changed), \
                                patch.object(catches, "run_image", return_value=result), \
                                self.assertRaises(ValueError):
                            destructors.observe(path, name, "original", receipt, [], {}, 1)
                    result.update(stdout=prefix + lifetime, exit_code=1 - int(control))
                    with patch.object(catches, "run_image", return_value=result), \
                            self.assertRaises(ValueError):
                        destructors.observe(path, name, "original", receipt, [], {}, 1)
                # Even a correct lifetime trace cannot hide a damaged FS chain
                # or a throw caller outside the source function.
                for index in (6, 8, 9, 10):
                    changed = values.copy()
                    changed[index] = 0
                    result = {"exit_code": 0, "stdout": "MULTICATCH " +
                              " ".join(f"{v:08X}" for v in changed) + "\n" + lifetime}
                    with self.subTest(index=index), \
                            patch.object(catches, "run_image", return_value=result), \
                            self.assertRaises(ValueError):
                        destructors.observe(path, case, "original", receipt, [], {}, 1)


if __name__ == "__main__":
    unittest.main()
