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
PATCH_ROOT = ROOT / "cmake/patches"
SCOPE_PATCHES = [PATCH_ROOT / name for name in (
    "llvm-aarch64-specifier-scope.patch",
    "llvm-aarch64-upstream-specifier-scope.patch",
)]
I386_PATCH = PATCH_ROOT / "llvm-i386-got-base.patch"
MACHO_PATCH = PATCH_ROOT / "llvm-macho-gotpcrel-data.patch"
PATCH_GROUPS = [SCOPE_PATCHES, [I386_PATCH], [MACHO_PATCH]]


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

    def patch(self, patch: Path, *, reverse: bool = False, apply: bool = False,
              source: bool = False) -> subprocess.CompletedProcess[str]:
        clean = self.environment.copy()
        for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR", "GIT_PREFIX"):
            clean.pop(key, None)
        args = ["git", "apply", "--no-index"]
        if reverse:
            args.append("--reverse")
        if not apply:
            args.append("--check")
        if source:
            args.append("-p4")
        return subprocess.run(
            [*args, str(patch)],
            cwd=self.source if source else self.overlay.parents[3], env=clean,
            text=True, capture_output=True, timeout=10,
        )

    def assert_corrected(self) -> None:
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for group in PATCH_GROUPS:
            checks = [self.patch(patch, reverse=True) for patch in group]
            self.assertTrue(any(check.returncode == 0 for check in checks),
                            "\n".join(check.stderr for check in checks))
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

    def test_independently_upstreamed_corrections_are_completed(self) -> None:
        # Derive the uncorrected source without depending on which corrections
        # the pinned fork has already adopted. Each mutation remains in this
        # disposable input; the actual LLVM checkout is never modified.
        for group in PATCH_GROUPS:
            for patch in group:
                if self.patch(patch, reverse=True, source=True).returncode == 0:
                    result = self.patch(patch, reverse=True, apply=True, source=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    break
            else:
                self.assertEqual(self.patch(group[0], source=True).returncode, 0)
        uncorrected = self.backend.read_bytes()
        for scope in [None, *SCOPE_PATCHES]:
            for i386 in (False, True):
                for macho in (False, True):
                    with self.subTest(scope=scope, i386=i386, macho=macho):
                        self.backend.write_bytes(uncorrected)
                        for patch in [scope, I386_PATCH if i386 else None,
                                      MACHO_PATCH if macho else None]:
                            if patch is not None:
                                result = self.patch(patch, apply=True, source=True)
                                self.assertEqual(result.returncode, 0, result.stderr)
                        self.original = self.backend.read_bytes()
                        self.assert_corrected()

    def test_unrecognized_architecture_guard_is_not_accepted_as_corrected(self) -> None:
        self.assert_corrected()
        data = self.overlay.read_bytes()
        self.assertIn(b"if (TT.isAArch64()", data)
        self.backend.write_bytes(data.replace(b"if (TT.isAArch64()",
                                               b"if (TT.isX86()", 1))
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match the scoped correction", result.stderr)

    def test_unrecognized_source_fails_without_a_success_stamp(self) -> None:
        self.backend.write_text("int unrecognized;\n", encoding="utf-8")
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match the scoped correction", result.stderr)
        self.assertFalse((self.build / "neverd-llvm-address-model/inputs.sha256").exists())


if __name__ == "__main__":
    unittest.main()
