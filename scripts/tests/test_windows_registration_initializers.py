import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from scripts import check_windows_registration_multiple_catch as catches

from scripts import check_windows_registration_nested_try as runner
from scripts import replay_windows_registration_nested_try as replay
from scripts import windows_registration_initializers as initializers
from scripts import windows_registration_initializers_relift as relift
from scripts.tests import test_windows_registration_copies as fixtures


class InitializerEvidenceTests(unittest.TestCase):
    def capture(self, root):
        capture = fixtures.CopyEvidenceTests().capture(
            root, forms=initializers.FORMS, cases=initializers.CASES)
        profile = initializers.profile(runner.validate_decompilation)
        capture.update(profile="initializers",
                       source_sha256=runner.file_digest(profile.source),
                       object_types_sha256=runner.file_digest(profile.types),
                       object_proof_sha256=runner.file_digest(profile.proof),
                       source_dependencies_sha256={
                           path.name: runner.file_digest(path) for path in profile.dependencies})
        capture["cases"] = [case for case in capture["cases"]
                            if case["case"] in initializers.CASES]
        return capture

    def test_constructor_proof_and_each_source_dependency_are_required(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            self.assertEqual(len(replay.validate_capture(root, capture)), 160)
            self.assertEqual(len(relift.validate_first(root, capture)), 160)
            for key in ("source_sha256", "object_types_sha256", "object_proof_sha256"):
                changed = copy.deepcopy(capture)
                changed[key] = "stale"
                with self.subTest(key=key), self.assertRaises(ValueError):
                    replay.validate_capture(root, changed)
            for dependency in capture["source_dependencies_sha256"]:
                changed = copy.deepcopy(capture)
                del changed["source_dependencies_sha256"][dependency]
                with self.subTest(dependency=dependency), self.assertRaises(ValueError):
                    replay.validate_capture(root, changed)
            for profile in (None, "copies", "objects", "destructors", "nested"):
                changed = copy.deepcopy(capture)
                changed["profile"] = profile
                with self.subTest(profile=profile), self.assertRaises(ValueError):
                    relift.validate_first(root, changed)

    def test_all_constructor_optimization_and_control_cases_must_execute(self):
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

    def test_direct_and_private_constructors_have_distinct_required_cases(self):
        for case in initializers.CASES:
            self.assertEqual(relift.entry_context(case), (0, 0, b"callback_parent"))
        self.assertIn("direct-nested-o0-llvm-fixed", initializers.CASES)
        self.assertIn("direct-rethrow-o1-llvm-fixed", initializers.CASES)
        self.assertIn("direct-secondary-o0-llvm-fixed", initializers.CASES)
        for case in ("secondary-o0-llvm-fixed", "unknown"):
            with self.subTest(case=case), self.assertRaises(ValueError):
                relift.entry_context(case)


    def test_secondary_construction_requires_both_object_lifetimes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.capture(root)
            case = "direct-secondary-o0-llvm-fixed"
            path = root / case / "original.exe"
            receipt = json.loads((path.parent / "contract.json").read_text())
            self.assertEqual(initializers.expected_copy(case), (4, 0x04937E40))
            self.assertEqual(initializers.expected_lifetime(case), (16, 0x542BF300))
            # Independent values and event hashes distinguish a fresh exception
            # from a rethrow, including destruction of the original caught one.
            values = (39, 28, 39, 39, 18, 39, 1, 12, 0x401010, 0x401020, 0x401030)
            prefix = "MULTICATCH " + " ".join(f"{value:08X}" for value in values) + "\n"
            traces = "COPY 00000004 04937E40\nLIFETIME 00000010 542BF300\n"
            for control in (False, True):
                name = case + ("-control" if control else "")
                for observer in (initializers.observe, relift.observe):
                    kwargs = {"source_section": ".text"} if observer is relift.observe else {}
                    result = {"exit_code": int(control), "stdout": prefix + traces}
                    with patch.object(catches, "run_image", return_value=result):
                        observer(path, name, "original", receipt, [], {}, 1, **kwargs)
                    malformed = ("", traces.replace("00000004", "00000003"),
                                 traces.replace("04937E40", "00000000"),
                                 traces.replace("00000010", "0000000C"),
                                 traces.replace("542BF300", "D50DA380"), traces * 2)
                    for changed in malformed:
                        result["stdout"] = prefix + changed
                        with self.subTest(control=control, output=changed, observer=observer), \
                                patch.object(catches, "run_image", return_value=result), \
                                self.assertRaises(ValueError):
                            observer(path, name, "original", receipt, [], {}, 1, **kwargs)
                    result.update(stdout=prefix + traces, exit_code=1 - int(control))
                    with patch.object(catches, "run_image", return_value=result), \
                            self.assertRaises(ValueError):
                        observer(path, name, "original", receipt, [], {}, 1, **kwargs)


if __name__ == "__main__":
    unittest.main()
