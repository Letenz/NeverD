from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from scripts import check_source_patch_pe_runtime as runtime


class PEProcessQualificationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="neverd-pe-qualification-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.report = self.root / "x64-runtime.json"
        self.args = argparse.Namespace(root=self.root, arch="x64", runner=None,
                                       expected_native_host="x64")
        # Header-only parser inputs. Mocked process launches below are tests of
        # evidence validation, never a native PE execution qualification.
        data = bytearray(128)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 0x3C, 64)
        data[64:68] = b"PE\0\0"
        struct.pack_into("<H", data, 68, 0x8664)
        self.manifest = {"schema": "neverd.source-patch.pe-runtime.v1",
                         "commit": "qualification-commit", "images": []}
        cases = [("original", "original", 9)]
        cases += [(case, artifact, expected)
                  for case, expected in (("nonleaf", 87), ("writable-pointer", 92),
                                         ("floating-stack", 93))
                  for artifact in ("ir", "object", "supplied-object")]
        for case, artifact, expected in cases:
            name = f"{case}-{artifact}.exe"
            (self.root / name).write_bytes(data)
            self.manifest["images"].append(
                {"architecture": "x64", "case": case, "artifact": artifact,
                 "path": name, "sha256": hashlib.sha256(data).hexdigest(),
                 "expected": expected})
        self.save_manifest()
        self.addCleanup(patch.stopall)
        patch("builtins.print").start()
        patch.dict("os.environ", {"GITHUB_SHA": "qualification-commit"}).start()
        self.native = patch.object(runtime, "native_machine", return_value=0x8664).start()
        self.launch = patch.object(runtime, "run", side_effect=lambda command, log, expected:
                                   subprocess.CompletedProcess(command, expected)).start()

    def save_manifest(self):
        (self.root / "manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def test_every_case_requires_three_observations(self):
        runtime.execute(self.args)
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(self.launch.call_count, 30)
        self.assertEqual(len(report["observations"]), 30)
        self.assertEqual(report["native_host"], "x64")
        self.assertEqual({row["actual"] for row in report["observations"]}, {9, 87, 92, 93})

    def test_failed_replay_retires_previous_success(self):
        runtime.execute(self.args)
        self.launch.side_effect = RuntimeError("wrong process exit code")
        with self.assertRaisesRegex(RuntimeError, "wrong process exit"):
            runtime.execute(self.args)
        self.assertFalse(self.report.exists())

    def test_commit_mismatch_cannot_reuse_a_success(self):
        self.report.write_text("stale success", encoding="utf-8")
        self.manifest["commit"] = "another-commit"
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, "different commit"):
            runtime.execute(self.args)
        self.launch.assert_not_called()
        self.assertFalse(self.report.exists())

    def test_manifest_cannot_choose_its_own_expected_result(self):
        self.manifest["images"][0]["expected"] = 0
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, "expected result mismatch"):
            runtime.execute(self.args)
        self.launch.assert_not_called()

    def test_repeated_case_cannot_replace_missing_coverage(self):
        self.manifest["images"][-1] = self.manifest["images"][0]
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, "missing or duplicate"):
            runtime.execute(self.args)
        self.launch.assert_not_called()

    def test_changed_image_cannot_execute(self):
        row = self.manifest["images"][0]
        (self.root / row["path"]).write_bytes(b"changed image")
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            runtime.execute(self.args)
        self.launch.assert_not_called()

    def test_wrong_machine_cannot_execute_even_with_matching_digest(self):
        row = self.manifest["images"][0]
        image = self.root / row["path"]
        data = bytearray(image.read_bytes())
        struct.pack_into("<H", data, 68, 0xAA64)
        image.write_bytes(data)
        row["sha256"] = runtime.digest(image)
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, "machine mismatch"):
            runtime.execute(self.args)
        self.launch.assert_not_called()

    def test_wrong_native_host_cannot_claim_coverage(self):
        self.native.return_value = 0xAA64
        with self.assertRaisesRegex(RuntimeError, "native machine"):
            runtime.execute(self.args)
        self.launch.assert_not_called()

    def test_external_runner_cannot_claim_native_coverage(self):
        self.args.runner = Path("external-runner")
        with self.assertRaisesRegex(ValueError, "cannot claim native"):
            runtime.execute(self.args)
        self.native.assert_not_called()
        self.launch.assert_not_called()


class ProcessResultTests(unittest.TestCase):
    def test_wrong_exit_retains_process_diagnostics_and_fails(self):
        with tempfile.TemporaryDirectory(prefix="neverd-pe-exit-") as directory:
            log = Path(directory) / "process.log"
            with self.assertRaisesRegex(RuntimeError, "returned 7, expected 9"):
                runtime.run([sys.executable, "-c", "print('observed failure'); raise SystemExit(7)"],
                            log, expected=9)
            self.assertIn("observed failure", log.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
