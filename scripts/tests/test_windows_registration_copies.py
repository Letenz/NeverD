import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from scripts import check_windows_registration_multiple_catch as catches
from scripts import check_windows_registration_nested_try as runner
from scripts import replay_windows_registration_nested_try as replay
from scripts import windows_registration_copies as copies
from scripts import windows_registration_copies_relift as relift
from scripts.tests import test_windows_registration_objects as fixtures


class CopyEvidenceTests(unittest.TestCase):
    def capture(self, root):
        capture = fixtures.ObjectEvidenceTests().capture(root)
        capture.update(profile="copies",
                       source_sha256=runner.file_digest(copies.SOURCE),
                       object_types_sha256=runner.file_digest(copies.TYPES),
                       object_proof_sha256=runner.file_digest(copies.PROOF),
                       source_dependencies_sha256={
                           copies.SHARED_DRIVER.name: runner.file_digest(copies.SHARED_DRIVER)})
        return capture

    def test_copy_matrix_binds_construction_source_and_proof(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            self.assertEqual(len(replay.validate_capture(root, capture)), 128)
            self.assertEqual(len(relift.validate_first(root, capture)), 128)
            for field, value in (("profile", "objects"), ("profile", "destructors"),
                                 ("source_sha256", "stale"),
                                 ("object_proof_sha256", "stale"),
                                 ("object_types_sha256", "stale"),
                                 ("source_dependencies_sha256", {}),
                                 ("source_dependencies_sha256", {
                                     copies.SHARED_DRIVER.name: "stale"})):
                changed = copy.deepcopy(capture)
                changed[field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    replay.validate_capture(root, changed)
            for profile in (None, "objects", "nested", "destructors"):
                changed = copy.deepcopy(capture)
                changed["profile"] = profile
                with self.subTest(profile=profile), self.assertRaises(ValueError):
                    relift.validate_first(root, changed)
            (root / capture["cases"][0]["case"] / "rewrite.xml").write_text(
                '<testsuites tests="2"><testcase/><testcase><skipped/></testcase></testsuites>')
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)

    def test_copy_observation_requires_both_lifetime_traces_and_control(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.capture(root)
            # Independent expected traces bind four real copy invocations and
            # twelve destructions to their actual object contents and order.
            traces = {"nested": (0x02630780, 0x8810D940),
                      "rethrow": (0x04937E40, 0xD50DA380)}
            for family, (constructed, destroyed) in traces.items():
                case = family + "-o0-llvm-fixed"
                path = root / case / "original.exe"
                receipt = json.loads((path.parent / "contract.json").read_text())
                values = ([39, 28, 39, 39, 18, 39, 1, 12] if family == "rethrow"
                          else [17, 28, 39, 7, 18, 39, 1, 12])
                values += [0x401010, 0x401020, 0x401030]
                prefix = "MULTICATCH " + " ".join(f"{v:08X}" for v in values) + "\n"
                construction = f"COPY 00000004 {constructed:08X}\n"
                destruction = f"LIFETIME 0000000C {destroyed:08X}\n"
                self.assertEqual(copies.expected_copy(case), (4, constructed))
                for control in (False, True):
                    name = case + ("-control" if control else "")
                    result = {"exit_code": int(control),
                              "stdout": prefix + construction + destruction}
                    with patch.object(catches, "run_image", return_value=result):
                        copies.observe(path, name, "original", receipt, [], {}, 1)
                        relift.observe(path, name, "original", receipt, [], {}, 1,
                                       source_section=".text")
                    malformed = ("", construction, destruction,
                                 construction.replace("00000004", "00000003") + destruction,
                                 construction.replace(f"{constructed:08X}", "00000000") + destruction,
                                 construction + destruction.replace("0000000C", "0000000B"),
                                 construction + destruction.replace(f"{destroyed:08X}", "00000000"),
                                 destruction + construction, construction * 2 + destruction,
                                 construction + destruction + "extra\n")
                    for changed in malformed:
                        result["stdout"] = prefix + changed
                        with self.subTest(family=family, control=control, output=changed), \
                                patch.object(catches, "run_image", return_value=result), \
                                self.assertRaises(ValueError):
                            copies.observe(path, name, "original", receipt, [], {}, 1)
                    result.update(stdout=prefix + construction + destruction,
                                  exit_code=1 - int(control))
                    with patch.object(catches, "run_image", return_value=result), \
                            self.assertRaises(ValueError):
                        copies.observe(path, name, "original", receipt, [], {}, 1)


if __name__ == "__main__":
    unittest.main()
