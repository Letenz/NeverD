"""The build overlay must change only its copy, regardless of Git's environment."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "third_party/llvm-project/llvm/lib/MC/AddressModelBackend.cpp"
HELPER = ROOT / "cmake/NeverDLLVMAddressModel.cmake"
PATCH = ROOT / "cmake/patches/llvm-aarch64-specifier-scope.patch"


@unittest.skipUnless(shutil.which("cmake") and shutil.which("git") and BACKEND.is_file(),
                     "CMake, Git and the pinned LLVM source are required")
class LLVMAddressModelOverlayTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="neverd-overlay-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.source = self.directory / "source"
        self.source.mkdir()
        self.backend = self.source / "AddressModelBackend.cpp"
        self.backend.write_bytes(BACKEND.read_bytes())
        self.original = self.backend.read_bytes()
        self.build = self.directory / "build"
        self.overlay = self.build / "neverd-llvm-address-model/llvm/lib/MC/AddressModelBackend.cpp"
        (self.source / "Other.cpp").write_text("int other;\n", encoding="utf-8")
        (self.source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\n'
            'project(OverlayCheck LANGUAGES CXX)\n'
            'add_library(LLVMMC OBJECT AddressModelBackend.cpp Other.cpp)\n'
            f'include("{HELPER.as_posix()}")\n'
            'neverd_scope_llvm_address_model()\n'
            'get_target_property(sources LLVMMC SOURCES)\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/sources.txt" "${sources}")\n',
            encoding="utf-8",
        )
        self.environment = os.environ.copy()
        for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR", "GIT_PREFIX"):
            self.environment.pop(key, None)

    def configure(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["cmake", "-S", str(self.source), "-B", str(self.build)],
            env=self.environment, text=True, capture_output=True, timeout=60,
        )

    def assert_corrected(self) -> None:
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        clean = self.environment.copy()
        for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR", "GIT_PREFIX"):
            clean.pop(key, None)
        check = subprocess.run(
            ["git", "apply", "--no-index", "--reverse", "--check", str(PATCH)],
            cwd=self.overlay.parents[3], env=clean,
            text=True, capture_output=True, timeout=10,
        )
        self.assertEqual(check.returncode, 0, check.stderr)
        self.assertEqual(self.backend.read_bytes(), self.original)
        sources = (self.build / "sources.txt").read_text().split(";")
        self.assertEqual(sources, [self.overlay.as_posix(), (self.source / "Other.cpp").as_posix()])

    def test_only_overlay_is_corrected_and_reconfigure_keeps_it(self) -> None:
        self.assert_corrected()
        before = self.overlay.stat().st_mtime_ns
        self.assert_corrected()
        self.assertEqual(self.overlay.stat().st_mtime_ns, before)

    def test_inherited_explicit_repository_cannot_skip_the_correction(self) -> None:
        self.environment["GIT_DIR"] = subprocess.check_output(
            ["git", "rev-parse", "--absolute-git-dir"], cwd=ROOT,
            env=self.environment, text=True,
        ).strip()
        self.environment["GIT_WORK_TREE"] = str(ROOT)
        self.environment["GIT_INDEX_FILE"] = str(self.directory / "foreign-index")
        self.environment["GIT_PREFIX"] = "unrelated/"
        self.assert_corrected()

    def test_already_corrected_source_is_accepted(self) -> None:
        self.assert_corrected()
        self.backend.write_bytes(self.overlay.read_bytes())
        self.original = self.backend.read_bytes()
        self.assert_corrected()

    def test_unrecognized_source_fails_without_a_success_stamp(self) -> None:
        self.backend.write_text("int unrecognized;\n", encoding="utf-8")
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match the scoped correction", result.stderr)
        self.assertFalse((self.build / "neverd-llvm-address-model/inputs.sha256").exists())


if __name__ == "__main__":
    unittest.main()
