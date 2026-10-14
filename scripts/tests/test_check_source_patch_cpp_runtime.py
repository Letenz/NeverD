from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from scripts import check_source_patch_cpp_runtime as checker


class CppRuntimeEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        data = bytearray(80)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 0x3c, 64)
        data[64:68] = b"PE\0\0"
        struct.pack_into("<H", data, 68, 0xaa64)
        self.manifest = {"schema": checker.SCHEMA, "commit": "verified-revision", "images": []}
        for kind in ("original", "patched"):
            image = self.root / (kind + ".exe")
            image.write_bytes(data)
            self.manifest["images"].append({"architecture": "arm64", "kind": kind,
                "path": image.name, "sha256": checker.pe.digest(image)})
        self.args = argparse.Namespace(root=self.root, arch="arm64", runner=None,
                                       expected_native_host="arm64")
        self.write_manifest()

    def write_manifest(self):
        (self.root / "manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def execute(self):
        with patch.dict(checker.os.environ, {"GITHUB_SHA": "verified-revision"}), \
             patch.object(checker.pe, "native_machine", return_value=0xaa64), \
             patch.object(checker.pe, "run", side_effect=lambda command, log, expected:
                          subprocess.CompletedProcess(command, expected)) as run:
            checker.execute(self.args)
            return run

    def test_complete_inventory_has_independent_scenarios_and_three_attempts(self):
        run = self.execute()
        self.assertEqual(run.call_count, 36)
        report = json.loads((self.root / "arm64-runtime.json").read_text())
        self.assertEqual(report["native_host"], "arm64")
        self.assertEqual(len(report["observations"]), 36)
        self.assertEqual({row["mode"] for row in report["observations"] if row["kind"] == "patched"},
                         {0, 1, 2, 3, 4, 5, 6, 7, 9})

    def test_wrong_commit_schema_hash_machine_inventory_and_path_fail(self):
        changes = [lambda: self.manifest.update(commit="other"),
                   lambda: self.manifest.update(schema="other"),
                   lambda: self.manifest["images"][0].update(sha256="0" * 64),
                   lambda: self.manifest["images"].pop(),
                   lambda: self.manifest["images"][0].update(path="../original.exe"),
                   lambda: self.manifest["images"][0].update(kind="patched")]
        baseline = json.dumps(self.manifest)
        for change in changes:
            with self.subTest(change=change):
                self.manifest = json.loads(baseline)
                change()
                self.write_manifest()
                (self.root / "arm64-runtime.json").write_text("stale-success")
                with self.assertRaises((ValueError, OSError)):
                    self.execute()
                self.assertFalse((self.root / "arm64-runtime.json").exists())
        self.manifest = json.loads(baseline)
        self.write_manifest()
        image = self.root / "original.exe"
        data = bytearray(image.read_bytes())
        struct.pack_into("<H", data, 68, 0x8664)
        image.write_bytes(data)
        self.manifest["images"][0]["sha256"] = checker.pe.digest(image)
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "machine"):
            self.execute()

    def test_arm64_requires_matching_native_host_and_runner_cannot_claim_native(self):
        with patch.object(checker.pe, "native_machine", return_value=0x8664):
            with self.assertRaisesRegex(RuntimeError, "native machine"):
                checker.execute(self.args)
        self.args.runner = Path("external")
        with self.assertRaisesRegex(ValueError, "cannot claim native"):
            checker.execute(self.args)

    def test_runtime_failure_cannot_leave_a_success_report(self):
        (self.root / "arm64-runtime.json").write_text("stale-success")
        with patch.dict(checker.os.environ, {"GITHUB_SHA": "verified-revision"}), \
             patch.object(checker.pe, "native_machine", return_value=0xaa64), \
             patch.object(checker.pe, "run", side_effect=RuntimeError("unwinder failed")):
            with self.assertRaisesRegex(RuntimeError, "unwinder"):
                checker.execute(self.args)
        self.assertFalse((self.root / "arm64-runtime.json").exists())


if __name__ == "__main__":
    unittest.main()
