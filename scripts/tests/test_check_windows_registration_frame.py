from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from scripts import check_windows_registration_frame as runner


class WindowsRegistrationFrameRunnerTests(unittest.TestCase):
    def observe(self, outcome):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "frame.obj"
            source.write_bytes(b"\x4c\x01" + bytes(18))
            with patch.object(runner.subprocess, "run", return_value=
                              subprocess.CompletedProcess([], 0, "", "")), \
                    patch.object(runner, "image_digest", return_value="image"), \
                    patch.object(runner, "run_image", return_value=outcome):
                return runner.build_and_observe(source, root, "lld-link",
                                                ["wine"], {}, 1)

    def test_runtime_success_retains_a_separate_abi_evidence_class(self):
        report = self.observe({"exit_code": 0, "stdout": runner.BANNER + "\n"})
        self.assertTrue(report["passed"])
        self.assertEqual(report["evidence"], "generated-x86-callback-abi")
        self.assertIn("object_sha256", report)
        self.assertEqual(len(report["link_steps"]), 3)
        self.assertFalse(any("safeseh:no" in part.lower()
                             for command in report["commands"] for part in command))

    def test_wrong_trace_does_not_pass_with_a_zero_exit_status(self):
        self.assertFalse(self.observe({"exit_code": 0, "stdout": "wrong"})["passed"])

    def test_a_crash_does_not_pass_with_the_success_banner(self):
        self.assertFalse(self.observe({"exit_code": 1,
                                       "stdout": runner.BANNER})["passed"])

    def test_a_non_i386_object_is_rejected_before_linking(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "frame.obj"
            source.write_bytes(b"\x64\x86" + bytes(18))
            with patch.object(runner.subprocess, "run") as execute:
                report = runner.build_and_observe(source, root, "lld-link", [], {}, 1)
            self.assertFalse(report["passed"])
            execute.assert_not_called()


if __name__ == "__main__":
    unittest.main()
