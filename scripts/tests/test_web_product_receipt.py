"""Behavioral probes for the required product publication boundary."""

import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import web_product_receipt as receipt
import web_product_release as release


class ProductReceiptTests(unittest.TestCase):
    def setUp(self):
        environment = patch.dict(os.environ, {"GITHUB_SERVER_URL": "https://github.com",
                                              "GITHUB_REPOSITORY": "NeverSight/NeverD"})
        environment.start()
        self.addCleanup(environment.stop)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.names = receipt.NATIVE_REQUIRED | {"WebSDK.AdditionalDiscoveredCase"}

    def junit(self, name, names):
        root = ET.Element("testsuite", tests=str(len(names)), failures="0", skipped="0")
        for case in sorted(names):
            ET.SubElement(root, "testcase", name=case, status="run")
        path = self.root / name
        ET.ElementTree(root).write(path, encoding="utf-8")
        return path

    def test_complete_discovery_passes_and_every_executed_case_is_required(self):
        path = self.junit("native.xml", self.names)
        self.assertEqual(receipt.audit_junit(path, self.names, receipt.NATIVE_REQUIRED)["passed"],
                         len(self.names))
        for bad in (self.names - {"WebSDK.AdditionalDiscoveredCase"}, self.names | {"foreign"}):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                receipt.audit_junit(self.junit("bad.xml", bad), self.names, receipt.NATIVE_REQUIRED)
        with self.assertRaises(ValueError):
            receipt.audit_junit(path, self.names - {next(iter(receipt.NATIVE_REQUIRED))}, receipt.NATIVE_REQUIRED)

    def test_skip_failure_error_disabled_and_duplicate_cannot_pass(self):
        for kind in ("skipped", "failure", "error", "disabled", "duplicate", "summary"):
            with self.subTest(kind=kind):
                path = self.junit("native.xml", self.names)
                tree = ET.parse(path)
                first = tree.getroot().find("testcase")
                if kind == "disabled":
                    first.set("status", "notrun")
                elif kind == "duplicate":
                    tree.getroot().append(ET.Element("testcase", first.attrib))
                elif kind == "summary":
                    tree.getroot().set("failures", "1")
                else:
                    ET.SubElement(first, kind)
                tree.write(path)
                with self.assertRaises(ValueError):
                    receipt.audit_junit(path, self.names, receipt.NATIVE_REQUIRED)

    def installed(self):
        return {"schema_version": 1, "status": "ok",
                "input_reader": "windows-ntfs-handle-v1",
                "artifact_storage": "windows-private-unlinked-spool-v1",
                "checks": sorted(receipt.INSTALLED_CHECKS)}

    def test_installed_checks_require_all_six_consumers_and_native_storage(self):
        path = self.root / "installed.json"
        path.write_text(json.dumps(self.installed()), encoding="utf-8-sig")
        self.assertEqual(receipt.audit_installed(path)["status"], "ok")
        for key, value in (("checks", ["cli"]), ("status", "failed"),
                           ("input_reader", "unavailable"), ("artifact_storage", "unavailable")):
            path.write_text(json.dumps(self.installed() | {key: value}))
            with self.subTest(key=key), self.assertRaises(ValueError):
                receipt.audit_installed(path)

    def test_publisher_reaudits_reports_and_rejects_changed_archive_source_or_run(self):
        commit = "a" * 40
        inventory = self.root / "windows-native-inventory.json"
        inventory.write_text(json.dumps({"tests": [{"name": n} for n in sorted(self.names)]}))
        native = receipt.audit_junit(self.junit("windows-native-results.xml", self.names),
                                     self.names, receipt.NATIVE_REQUIRED)
        native["inventory_sha256"] = receipt.digest(inventory)
        adapters = receipt.audit_junit(self.junit("windows-adapter-results.xml", receipt.ADAPTER_REQUIRED),
                                       receipt.ADAPTER_REQUIRED, receipt.ADAPTER_REQUIRED)
        installed_path = self.root / "windows-installed-results.json"
        installed_path.write_text(json.dumps(self.installed()))
        archive = self.root / f"neverd-web-windows-x64-{commit[:12]}.zip"
        archive.write_bytes(b"qualified bytes")
        value = {"schema_version": 1, "status": "qualified", "source_commit": commit,
                 "qualification_url": "https://github.com/NeverSight/NeverD/actions/runs/123",
                 "native": native, "adapters": adapters,
                 "installed": receipt.audit_installed(installed_path),
                 "archive": {"name": archive.name, "bytes": archive.stat().st_size,
                             "sha256": receipt.digest(archive)}}
        path = self.root / "windows-product-receipt.json"
        path.write_text(json.dumps(value))
        output = self.root / "published"
        release.prepare(self.root, output, commit, "123")
        self.assertEqual((output / archive.name).read_bytes(), b"qualified bytes")
        for source, run in (("b" * 40, "123"), (commit, "124")):
            with self.assertRaises(ValueError):
                release.prepare(self.root, self.root / "rejected", source, run)
        archive.write_bytes(b"different bytes")
        with self.assertRaises(ValueError):
            release.prepare(self.root, self.root / "rejected", commit, "123")
        archive.write_bytes(b"qualified bytes")
        self.junit("windows-native-results.xml", self.names - {"WebSDK.AdditionalDiscoveredCase"})
        with self.assertRaises(ValueError):
            release.prepare(self.root, self.root / "rejected", commit, "123")


if __name__ == "__main__":
    unittest.main()
