import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

from scripts import audit_hvf_shards as audit
from scripts import run_native_cpu_ci as native
from scripts import run_native_cpu_methods as methods


@unittest.skipUnless(os.name == "posix", "method execution requires POSIX")
class HVFShardEvidenceTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.commit = "a" * 40
        (self.root / "scripts").mkdir()
        (self.root / "scripts/NativeCPUTests.def").write_text("NEVERD_NATIVE_CPU_OUTPUT_LIMIT(65536)\n")
        (self.root / "scripts/NativeHVFTests.def").write_text(
            'NEVERD_NATIVE_HVF_OWNER(OwnerA)\nNEVERD_NATIVE_HVF_OWNER(OwnerB)\n'
            'NEVERD_NATIVE_HVF_REQUIRED_TEST("Native.Execute/0")\n'
            'NEVERD_NATIVE_HVF_REQUIRED_TEST("Other.Execute/1")\n')
        self.build = self.root / "build"
        (self.build / "CMakeFiles").mkdir(parents=True)
        (self.build / "CMakeFiles/TargetDirectories.txt").write_text("\n".join(
            str(self.build / f"unittests/emulation/CMakeFiles/{owner}.dir") for owner in ("OwnerA", "OwnerB")))
        self.document = {"kind": "ctestInfo", "version": {"major": 1}, "tests": []}
        for owner, family in (("OwnerA", "Native.Execute"), ("OwnerA", "Native.Mutate"),
                              ("OwnerB", "Other.Execute"), ("OwnerB", "Optional.Case")):
            for index in range(2):
                name = family + "/" + str(index)
                self.document["tests"].append({"name": name, "command": [
                    str(self.build / "bin" / owner), "--gtest_filter=" + name,
                    "--gtest_also_run_disabled_tests"], "properties": [
                        {"name": "LABELS", "value": [owner]},
                        {"name": "ENVIRONMENT", "value": ["NEVERD_SIGNATURE_CACHE=off"]},
                        {"name": "WORKING_DIRECTORY", "value": str(self.build / "unittests/emulation")},
                        {"name": "SKIP_REGULAR_EXPRESSION", "value": [r"\[  SKIPPED \]"]},
                        {"name": "TIMEOUT", "value": 20.0}]})
        self.paths = [self.root / f"shard-{index}" for index in range(2)]
        for index, path in enumerate(self.paths):
            self.assertEqual(self.collect(path, index), 0)

    def capture(self, command, **kwargs):
        if command[0] == "git":
            return self.commit if command[1] == "rev-parse" else ""
        return json.dumps(self.document)

    def execute(self, command, directory, environment, timeout, evidence):
        evidence.mkdir(parents=True)
        names = command[1].removeprefix("--gtest_filter=").split(":")
        root = ET.Element("testsuites", tests=str(len(names)), failures="0", disabled="0", errors="0")
        suite = ET.SubElement(root, "testsuite")
        for name in names:
            classname, method = name.split(".", 1)
            case = ET.SubElement(suite, "testcase", classname=classname, name=method,
                                 status="run", result="completed")
            if classname == "Optional":
                case.set("result", "skipped")
                ET.SubElement(case, "skipped", message="foreign ISA")
        ET.ElementTree(root).write(evidence / "results.xml")
        status = {"command": command, "working_directory": directory, "timeout_seconds": timeout,
                  "status": 0, "timed_out": False, "child_retired": True,
                  "native_requirements": {"NEVERD_REQUIRE_HVF": environment["NEVERD_REQUIRE_HVF"]},
                  "test_environment": {"NEVERD_SIGNATURE_CACHE": environment["NEVERD_SIGNATURE_CACHE"]}}
        (evidence / "status.json").write_text(json.dumps(status))
        return status

    def collect(self, path, index):
        with mock.patch.object(native, "ROOT", self.root), \
                mock.patch.object(native.platform, "machine", return_value="x86_64"), \
                mock.patch.object(native.platform, "system", return_value="Darwin"), \
                mock.patch.object(native.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)), \
                mock.patch.object(native.subprocess, "check_output", side_effect=self.capture), \
                mock.patch.object(methods, "execute", side_effect=self.execute), \
                contextlib.redirect_stdout(io.StringIO()):
            return native.run(self.build, path, 4, False, require_hvf=True,
                              execution_methods=True, hvf_shard=(index, 2))

    def check(self, paths=None):
        return audit.audit_shards(self.paths if paths is None else paths, self.commit, "x86_64", 2, self.root)

    def mutate(self, path, change):
        original = path.read_bytes()
        document = json.loads(original)
        change(document)
        path.write_text(json.dumps(document))
        try:
            with self.assertRaises((ValueError, KeyError)):
                self.check()
        finally:
            path.write_bytes(original)

    def test_complete_raw_evidence_reconciles_in_either_shard_order(self):
        result = self.check()
        self.assertTrue(result["passed"])
        self.assertEqual(result["registered"], 8)
        self.assertEqual(result["counts"], {"passed": 6, "skipped": 2, "failed": 0,
                                           "disabled": 0, "not_run": 0})
        self.assertEqual(result["required_native_tests"], 2)
        self.assertEqual(result, self.check(list(reversed(self.paths))))

    def test_missing_and_duplicate_shards_are_rejected(self):
        for paths in (self.paths[:1], self.paths + self.paths[:1], [self.paths[0]] * 2):
            with self.subTest(paths=paths), self.assertRaises(ValueError):
                self.check(paths)

    def test_source_host_profile_and_claimed_counts_cannot_replace_raw_evidence(self):
        for field, value in (("commit", "b" * 40), ("source_dirty", True),
                             ("host_architecture", "arm64"), ("require_hvf", False),
                             ("hvf_transport_only", True), ("total", 999),
                             ("owners", ["OwnerA"]), ("required_native_names", []),
                             ("required_native_tests", 0), ("execution_status", 1)):
            with self.subTest(field=field):
                self.mutate(self.paths[0] / "summary.json", lambda data: data.__setitem__(field, value))

    def test_full_inventory_contract_drift_is_rejected(self):
        path = self.paths[0] / "full-inventory.json"
        self.mutate(path, lambda data: data["tests"].pop())
        self.mutate(path, lambda data: data["tests"][0]["properties"][-1].__setitem__("value", 99))

    def test_coherent_shard_cannot_relabel_a_different_parameter_as_the_original(self):
        for test in self.document["tests"]:
            if test["name"] == "Native.Execute/1":
                test["command"][1] = "--gtest_filter=Native.Execute/9"
        changed = self.root / "relabelled-shard"
        self.assertEqual(self.collect(changed, 0), 0)
        with self.assertRaisesRegex(ValueError, "full inventory execution contracts"):
            self.check([changed, self.paths[1]])

    def test_selected_inventory_cannot_drop_or_duplicate_optional_parameters(self):
        path = self.paths[0] / "inventory.json"
        self.mutate(path, lambda data: data["tests"].pop())
        self.mutate(path, lambda data: data["tests"].append(data["tests"][0]))

    def test_process_failure_timeout_native_override_and_contract_changes_are_rejected(self):
        path = self.paths[0] / "methods/0000/status.json"
        for field, value in (("status", -9), ("timed_out", True), ("child_retired", False),
                             ("native_requirements", {"NEVERD_REQUIRE_HVF": "0"}),
                             ("test_environment", {"NEVERD_SIGNATURE_CACHE": "on"}),
                             ("working_directory", "/other"), ("timeout_seconds", 999)):
            with self.subTest(field=field):
                self.mutate(path, lambda data: data.__setitem__(field, value))
        self.mutate(path, lambda data: data["command"].__setitem__(1, "--gtest_filter=Native.*"))

    def test_mapping_report_and_plan_cannot_disagree_with_original_xml(self):
        self.mutate(self.paths[0] / "methods/0000/identities.json", lambda data: data.clear())
        self.mutate(self.paths[0] / "method-results.json", lambda data: data["results"].pop())
        self.mutate(self.paths[0] / "method-plan.json", lambda data: data.pop())

    def test_required_native_skip_fails_even_with_other_native_successes(self):
        path = self.paths[0] / "methods/0000/results.xml"
        original = path.read_bytes()
        document = ET.fromstring(original)
        case = next(document.iter("testcase"))
        self.assertEqual(case.get("classname") + "." + case.get("name"), "Native.Execute/0")
        case.set("result", "skipped")
        ET.SubElement(case, "skipped", message="native unavailable")
        ET.ElementTree(document).write(path)
        with self.assertRaisesRegex(ValueError, "native coverage was skipped"):
            self.check()

    def test_missing_raw_xml_cannot_be_replaced_by_successful_summary(self):
        (self.paths[0] / "methods/0000/results.xml").unlink()
        with self.assertRaises(OSError):
            self.check()

    def test_missing_required_registration_fails_before_any_shard_executes(self):
        self.document["tests"] = [test for test in self.document["tests"]
                                  if test["name"] != "Other.Execute/1"]
        with self.assertRaisesRegex(ValueError, "missing required native HVF"):
            self.collect(self.root / "missing", 0)

    def test_sharding_cannot_weaken_the_transport_or_non_hvf_profile(self):
        for options in ({}, {"require_hvf": True},
                        {"require_hvf": True, "execution_methods": True, "hvf_transport_only": True}):
            with self.subTest(options=options), self.assertRaisesRegex(ValueError, "complete HVF profile"):
                native.run(self.build, self.root / "invalid", 1, False,
                           hvf_shard=(0, 2), **options)


if __name__ == "__main__":
    unittest.main()
