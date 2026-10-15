from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zlib

from scripts import check_source_patch_additional_runtime as checker
from scripts import check_source_patch_snapshot_runtime as snapshot


class ExecutionIdentityTests(unittest.TestCase):
    def test_rosetta_is_never_a_matching_intel_host(self):
        with patch.object(checker.platform, "system", return_value="Darwin"), \
             patch.object(checker.platform, "machine", return_value="x86_64"), \
             patch.object(checker.subprocess, "run", return_value=
                          subprocess.CompletedProcess([], 0, stdout="1\n")):
            identity = checker.runtime_identity("macos", "x64", None)
        self.assertEqual(identity["host_architecture"], "arm64")
        self.assertEqual(identity["execution_kind"], "rosetta")

    def test_32_bit_processes_retain_the_64_bit_host_identity(self):
        with patch.object(checker.platform, "system", return_value="Linux"), \
             patch.object(checker.platform, "machine", return_value="x86_64"):
            identity = checker.runtime_identity("linux", "x86", None)
        self.assertEqual(identity["host_architecture"], "x64")
        self.assertEqual(identity["execution_kind"], "compatibility")

    def test_arm32_cannot_claim_a_native_arm64_execution(self):
        with patch.object(checker.platform, "system", return_value="Linux"), \
             patch.object(checker.platform, "machine", return_value="aarch64"):
            with self.assertRaisesRegex(RuntimeError, "cannot execute directly"):
                checker.runtime_identity("linux", "arm32", None)
            with self.assertRaisesRegex(ValueError, "declared Linux QEMU"):
                checker.runtime_identity("linux", "arm32", Path("qemu-i386"))
            with patch.object(checker.subprocess, "run", return_value=
                              subprocess.CompletedProcess([], 0, stdout="qemu-arm version 10\n")):
                identity = checker.runtime_identity("linux", "arm32", Path("qemu-arm"))
        self.assertEqual(identity["execution_kind"], "qemu-user")
        self.assertEqual(identity["host_architecture"], "arm64")


class ProcessEvidenceTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.args = argparse.Namespace(root=self.root, system="linux", arch="x86",
                                       runner=None, execution="compatibility")
        # Parser/evidence fixtures and mocked launches are not OS runtime proof.
        data = bytearray(52)
        data[:6] = b"\x7fELF\x01\x01"
        struct.pack_into("<HH", data, 16, 2, 3)
        self.manifest = {"schema": checker.SCHEMA, "commit": "checked-commit",
            "engine_source_commit": "checked-engine", "engine_source_digest": "digest",
            "producer_commit": "recorded-producer", "images": []}
        for case in checker.cases("linux", "x86"):
            path = self.root / (case + ".elf")
            path.write_bytes(data)
            self.manifest["images"].append({"architecture": "x86", "case": case,
                "path": path.name, "sha256": checker.pe.digest(path)})
        self.save()
        self.addCleanup(patch.stopall)
        patch("builtins.print").start()
        patch.dict(checker.os.environ, {"GITHUB_SHA": "checked-commit"}).start()
        patch.object(checker.platform, "system", return_value="Linux").start()
        patch.object(checker.platform, "machine", return_value="x86_64").start()
        self.launch = patch.object(checker.pe, "run", side_effect=lambda command, log, expected:
            subprocess.CompletedProcess(command, expected)).start()

    def save(self):
        (self.root / "manifest.json").write_text(json.dumps(self.manifest))

    def test_complete_inventory_runs_each_independent_observation_three_times(self):
        self.manifest["images"][0]["expected"] = 123  # Untrusted manifest oracle.
        self.save()
        report = checker.execute(self.args)
        self.assertEqual(len(report["observations"]), 48)
        self.assertEqual(report["execution_kind"], "compatibility")
        self.assertEqual(report["host_architecture"], "x64")
        original = [row for row in report["observations"] if row["case"] == "abi-original"]
        self.assertEqual([(row["expected"], row["actual"]) for row in original], [(9, 9)] * 3)
        self.assertEqual(self.launch.call_count, 48)

    def test_duplicate_missing_foreign_and_changed_images_fail_before_launch(self):
        baseline = json.loads(json.dumps(self.manifest))
        for mutate in (lambda m: m["images"].pop(),
                       lambda m: m["images"].append(m["images"][0]),
                       lambda m: m["images"][0].update(architecture="arm32"),
                       lambda m: m["images"][0].update(path="../outside.elf"),
                       lambda m: m["images"][0].update(sha256="changed"),
                       lambda m: m.update(commit="another-commit")):
            self.manifest = json.loads(json.dumps(baseline))
            mutate(self.manifest)
            self.save()
            with self.assertRaises(ValueError):
                checker.execute(self.args)
        self.launch.assert_not_called()

    def test_a_failure_or_wrong_execution_kind_removes_an_earlier_success(self):
        report = self.root / "x86-runtime.json"
        for fail_identity in (False, True):
            report.write_text("old success")
            self.args.execution = "native" if fail_identity else "compatibility"
            self.launch.side_effect = RuntimeError("process failed")
            with self.assertRaises(RuntimeError):
                checker.execute(self.args)
            self.assertFalse(report.exists())

    def test_an_explicit_loader_cannot_be_reported_as_direct_host_execution(self):
        self.args.loader = self.root / self.manifest["images"][0]["path"]
        with self.assertRaisesRegex(ValueError, "explicit loader requires QEMU"):
            checker.execute(self.args)
        self.launch.assert_not_called()

    def test_qemu_report_retains_the_explicit_loader_and_sysroot(self):
        self.args.runner = Path("qemu-i386")
        self.args.execution = "qemu-user"
        self.args.sysroot = self.root
        self.args.loader = self.root / self.manifest["images"][0]["path"]
        with patch.object(checker.subprocess, "run", return_value=
                          subprocess.CompletedProcess([], 0, stdout="qemu-i386 version 8\n")):
            report = checker.execute(self.args)
        self.assertEqual(report["execution_kind"], "qemu-user")
        self.assertEqual(report["loader"], str(self.args.loader))
        self.assertEqual(report["runner_arguments"], ["-L", str(self.root)])
        self.assertEqual(self.launch.call_args_list[0].args[0][:4],
                         ["qemu-i386", "-L", str(self.root), str(self.args.loader)])


class SnapshotProvenanceTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.fixtures = self.root / "fixtures"
        self.fixtures.mkdir()
        original = self.fixtures / "original"
        original.write_bytes(b"authenticated image")
        digest = checker.pe.digest(original)
        delta = self.fixtures / "delta.json"
        delta.write_text(json.dumps({"size": original.stat().st_size,
            "original_sha256": digest, "sha256": digest, "writes": []}))
        receipt = json.dumps({"schema": "neverd.source-patch.v1", "success": True,
                              "compiler": 1, "language": 0}).encode()
        self.manifest = {"schema": snapshot.SCHEMA, "producer_commit": "recorded-binary",
            "engine_source_digest": "engine", "engine_source_commit": "engine-commit",
            "source_sha256": {}, "original_paths": "fixture", "images": [
                {"system": "linux", "architecture": "x86", "case": "patch",
                 "original": "original", "delta": "delta.json", "delta_sha256": checker.pe.digest(delta),
                 "receipts": [{"sha256": hashlib.sha256(receipt).hexdigest(),
                     "data": base64.b64encode(zlib.compress(receipt)).decode()}]}]}
        self.profile = snapshot.RuntimeProfile("test-only", {"patch": ((None, 1),)},
            ".image", lambda path: 3, 3, frozenset())

    def materialize(self):
        (self.fixtures / "manifest.json").write_text(json.dumps(self.manifest))
        with patch.object(snapshot, "engine_digest", return_value="engine"):
            snapshot.prepare(self.root, self.fixtures, self.root / "output", "linux", "x86",
                             profile=self.profile, producer_commit=None)

    def test_a_replacement_without_its_receipt_cannot_be_materialized(self):
        self.manifest["images"][0]["receipts"] = []
        with self.assertRaisesRegex(ValueError, "no publication receipt"):
            self.materialize()
        self.assertFalse((self.root / "output/manifest.json").exists())

    def test_successful_c_provenance_retains_the_exact_receipt(self):
        self.materialize()
        receipt = json.loads((self.root / "output/patch-0-receipt.json").read_text())
        self.assertEqual((receipt["compiler"], receipt["language"]), (1, 0))
        self.assertTrue((self.root / "output/manifest.json").is_file())

    def test_neverc_cpp_receipts_cannot_qualify_as_c_producer_evidence(self):
        receipt = json.dumps({"schema": "neverd.source-patch.v1", "success": True,
                              "compiler": 1, "language": 1}).encode()
        self.manifest["images"][0]["receipts"] = [{
            "sha256": hashlib.sha256(receipt).hexdigest(),
            "data": base64.b64encode(zlib.compress(receipt)).decode()}]
        with self.assertRaisesRegex(ValueError, "not successful"):
            self.materialize()
        self.assertFalse((self.root / "output/manifest.json").exists())

    def test_a_changed_pinned_producer_is_rejected_before_materialization(self):
        self.manifest["producer_inputs"] = {"fixtures/original": "changed"}
        with self.assertRaisesRegex(ValueError, "producer input"):
            self.materialize()
        self.assertFalse((self.root / "output").exists())


class MachOCodeExtentTests(unittest.TestCase):
    def test_hosted_comparison_requires_file_backed_executable_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "image"
            data = bytearray(136)
            struct.pack_into("<IIIIIIII", data, 0, 0xfeedfacf, 0x100000c, 0, 2, 1, 72, 0, 0)
            struct.pack_into("<II", data, 32, 0x19, 72)
            struct.pack_into("<QQQQ", data, 56, 0x1000, 32, 104, 32)
            struct.pack_into("<I", data, 92, 5)
            data[104:] = bytes(range(32))
            path.write_bytes(data)
            self.assertEqual(checker.macho_bytes(path, 0x1004, 6), bytes(range(4, 10)))
            for va, size in ((0xfff, 1), (0x101f, 2), (0x1000, 0)):
                with self.assertRaises(ValueError):
                    checker.macho_bytes(path, va, size)
            struct.pack_into("<I", data, 92, 3)
            path.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "unique executable"):
                checker.macho_bytes(path, 0x1004, 6)


if __name__ == "__main__":
    unittest.main()
