from pathlib import Path
import subprocess
import struct
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
                    patch.object(runner, "undecorate_kernel32_imports"), \
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

    def test_stdcall_import_kind_preserves_archive_offsets_and_symbols(self):
        archive = bytearray(b"!<arch>\n")
        offsets = []
        for name in (b"_RaiseException@16", b"_ExitProcess@4"):
            names = name + b"\0kernel32.dll\0"
            member = struct.pack("<HHHHIIHH", 0, 0xffff, 0, 0x14c,
                                 0, len(names), 0, 1 << 2) + names
            header = (f"import/         {0:<12}{0:<6}{0:<6}{0:<8}"
                      f"{len(member):<10}`\n").encode()
            self.assertEqual(len(header), 60)
            offsets.append(len(archive) + 60 + 18)
            archive.extend(header + member + (b"\n" if len(member) & 1 else b""))
        with tempfile.TemporaryDirectory() as directory:
            library = Path(directory) / "kernel32.lib"
            library.write_bytes(archive)
            runner.undecorate_kernel32_imports(library)
            result = library.read_bytes()
        expected = bytearray(archive)
        for offset in offsets:
            struct.pack_into("<H", expected, offset, 3 << 2)
        self.assertEqual(result, expected)

    def test_missing_stdcall_import_does_not_admit_an_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            library = Path(directory) / "kernel32.lib"
            library.write_bytes(b"!<arch>\n")
            with self.assertRaisesRegex(ValueError, "incomplete"):
                runner.undecorate_kernel32_imports(library)
            self.assertEqual(library.read_bytes(), b"!<arch>\n")


if __name__ == "__main__":
    unittest.main()
