"""Mutation tests for evidence validation; synthetic records are not execution."""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from scripts import check_source_patch_expansion_runtime as runtime


class NativeTransportTests(unittest.TestCase):
    def test_native_architecture_accepts_windows_spelling_and_refuses_32_bit(self):
        for name, expected in (
            ("AMD64", "x64"),
            ("x86_64", "x64"),
            ("ARM64", "arm64"),
            ("arm64", "arm64"),
            ("aarch64", "arm64"),
            ("i386", None),
            ("armv7l", None),
            ("unknown", None),
        ):
            with self.subTest(machine=name):
                self.assertEqual(runtime.native_architecture(name), expected)

    def test_source_receipt_hash_uses_exact_utf8_api_input(self):
        code = "// 原实现\nint replacement(int x) { return x + 1; }\n"
        payload = code.encode("utf-8")
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "replacement.c"
            runtime.write_patch_source(source, code)
            self.assertEqual(source.read_bytes(), payload)
            self.assertEqual(
                runtime.digest(source), hashlib.sha256(payload).hexdigest()
            )

    def test_process_paths_follow_evidence_os_on_any_audit_host(self):
        self.assertEqual(
            runtime.process_image_name(r"D:\work\native\callback-ir.exe", "Windows"),
            "callback-ir.exe",
        )
        self.assertEqual(
            runtime.process_image_name("/tmp/native/callback-ir", "Linux"),
            "callback-ir",
        )
        self.assertEqual(
            runtime.process_image_name(r"/tmp/native/back\slash", "Darwin"),
            r"back\slash",
        )


class ExpansionEvidenceTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="neverd-expansion-audit-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.report = {
            "schema": runtime.SCHEMA,
            "execution": "native-os-process",
            "system": "Linux",
            "architecture": "x64",
            "images": {},
            "providers": {},
            "observations": [],
            "loader_rejections": {},
        }
        self.populate(self.root, self.report, False)
        exception_root = self.root / "exceptions"
        exception_root.mkdir()
        exception = {"images": {}, "providers": {}, "observations": []}
        self.populate(exception_root, exception, True)
        self.save(exception_root / "report.json", exception)
        self.report["exceptions"] = exception
        for name in ("missing-library", "missing-symbol"):
            self.image(self.root, self.report["loader_rejections"], name)
            self.report["loader_rejections"][name]["returncode"] = 127
            self.save(
                self.root / (name + "-process.json"),
                {
                    "returncode": 127,
                    "stdout": "",
                    "stderr": "error while loading shared libraries"
                    if name == "missing-library"
                    else "undefined symbol",
                },
            )
        self.refresh()

    def save(self, path, value):
        path.write_text(json.dumps(value), encoding="utf-8")

    def image(self, root, rows, name):
        (root / name).write_bytes(b"synthetic evidence validator fixture")
        rows[name] = {"path": name, "sha256": runtime.digest(root / name)}

    def populate(self, root, report, exception):
        for name in ("old", "new"):
            self.image(root, report["providers"], name)
        for variant in ("original", *runtime.VARIANTS):
            self.image(root, report["images"], variant)
            if variant != "original":
                source = root / (
                    variant + ("-source.cpp" if exception else "-source.c")
                )
                source.write_text("synthetic validator source")
                callback = variant.startswith(("callback-", "combined-"))
                receipt = {
                    "schema": "neverd.source-patch.v1",
                    "success": True,
                    "target": "x86_64-unknown-linux-gnu",
                    "input_kind": 0,
                    "artifact": int(variant.endswith("-object")),
                    "source_sha256": runtime.digest(source),
                    "installed": [{}] * (1 if exception or not callback else 4),
                    "original_callbacks": [{}]
                    * ((1 if exception else 4) if callback else 0),
                    "added_imports": []
                    if variant.startswith("callback-")
                    else [{}] * (1 if exception else 2),
                }
                self.save(root / (variant + "-receipt.json"), receipt)
            for x in runtime.ARGUMENTS:
                for attempt in range(3):
                    actual = (
                        runtime.exception_expected if exception else runtime.expected
                    )(variant, x)
                    row = {
                        "variant": variant,
                        "argument": x,
                        "attempt": attempt,
                        "returncode": 0,
                        "actual": actual,
                    }
                    if not exception:
                        row["entry_address"] = 4096
                    report["observations"].append(row)
                    self.save(
                        root / f"{variant}-{x}-{attempt}.json",
                        {
                            "command": ["/synthetic/" + variant, str(x)],
                            "returncode": 0,
                            "stdout": " ".join(map(str, actual))
                            + ("" if exception else " 0x1000")
                            + "\n",
                            "stderr": "",
                        },
                    )

    def refresh(self):
        self.report["evidence_files"] = {
            p.relative_to(self.root).as_posix(): runtime.digest(p)
            for p in self.root.rglob("*")
            if p.is_file() and p != self.root / "report.json"
        }
        self.save(self.root / "report.json", self.report)

    def reject(self, expression):
        self.refresh()
        with self.assertRaisesRegex(ValueError, expression):
            runtime.audit(self.root)

    def test_complete_synthetic_inventory_validates(self):
        self.assertEqual(len(runtime.audit(self.root)["observations"]), 84)

    def test_windows_evidence_can_be_audited_on_another_os(self):
        self.report["system"] = "Windows"
        for root in (self.root, self.root / "exceptions"):
            for variant in runtime.VARIANTS:
                path = root / (variant + "-receipt.json")
                receipt = json.loads(path.read_text())
                receipt["target"] = "x86_64-pc-windows-msvc19.45.0"
                self.save(path, receipt)
            for variant in ("original", *runtime.VARIANTS):
                for x in runtime.ARGUMENTS:
                    for attempt in range(3):
                        path = root / f"{variant}-{x}-{attempt}.json"
                        log = json.loads(path.read_text())
                        log["command"][0] = "D:\\native\\" + variant
                        self.save(path, log)
        for name, code in (
            ("missing-library", 0xC0000135),
            ("missing-symbol", 0xC0000139),
        ):
            self.report["loader_rejections"][name]["returncode"] = code
            self.save(self.root / (name + "-process.json"), {"returncode": code})
        self.refresh()
        self.assertEqual(len(runtime.audit(self.root)["observations"]), 84)

    def test_missing_process_cannot_be_replaced_by_duplicate(self):
        self.report["observations"][-1] = copy.deepcopy(self.report["observations"][0])
        self.reject("duplicate")

    def test_failed_process_cannot_reuse_success_summary(self):
        path = self.root / "combined-object-25-2.json"
        data = json.loads(path.read_text())
        data["returncode"] = 1
        self.save(path, data)
        self.reject("process log differs")

    def test_log_output_must_match_independent_expected_values(self):
        self.report["observations"][0]["actual"][0] += 1
        self.reject("failed observation")

    def test_changed_binary_cannot_keep_old_digest(self):
        (self.root / "callback-ir").write_bytes(b"altered")
        self.reject("image/provider digest")

    def test_missing_callback_receipt_cannot_qualify(self):
        path = self.root / "combined-object-receipt.json"
        data = json.loads(path.read_text())
        data["original_callbacks"] = []
        self.save(path, data)
        self.reject("publication receipt")

    def test_wrong_target_cannot_qualify(self):
        path = self.root / "callback-ir-receipt.json"
        data = json.loads(path.read_text())
        data["target"] = "aarch64-unknown-linux-gnu"
        self.save(path, data)
        self.reject("receipt target")

    def test_ir_receipt_cannot_claim_object_coverage(self):
        path = self.root / "dependency-object-receipt.json"
        data = json.loads(path.read_text())
        data["artifact"] = 0
        self.save(path, data)
        self.reject("receipt target or artifact")

    def test_missing_exception_cleanup_is_not_success(self):
        path = self.root / "exceptions" / "combined-object-0-1.json"
        data = json.loads(path.read_text())
        data["stdout"] = "1 -20 0\n"
        self.save(path, data)
        self.reject("exception process")

    def test_missing_exception_attempt_is_not_success(self):
        path = self.root / "exceptions" / "report.json"
        data = json.loads(path.read_text())
        data["observations"].pop()
        self.save(path, data)
        self.reject("exception execution inventory")

    def test_crash_is_not_a_missing_library_diagnostic(self):
        path = self.root / "missing-library-process.json"
        self.save(path, {"returncode": -11, "stderr": "segmentation fault"})
        self.report["loader_rejections"]["missing-library"]["returncode"] = -11
        self.reject("native loader rejection")

    def test_digest_inventory_cannot_silently_omit_a_log(self):
        self.report["evidence_files"].pop("callback-ir-0-0.json")
        self.save(self.root / "report.json", self.report)
        with self.assertRaisesRegex(ValueError, "evidence file inventory"):
            runtime.audit(self.root)


if __name__ == "__main__":
    unittest.main()
