import copy
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock

from scripts import diagnose_hvf_methods as diagnostic
from scripts import run_native_cpu_methods as runner


class DiagnosticMethodsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.build = self.root / "build"
        self.build.mkdir()
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root}\n"
            "CMAKE_BUILD_TYPE:STRING=Release\n"
            "NEVERD_EMULATION_BACKEND_HVF:BOOL=ON\n"
            "NEVERD_EMULATION_BACKEND_UNICORN:BOOL=OFF\n"
            "NEVERD_LLVM_PREBUILT:BOOL=OFF\n")
        self.document = {"kind": "ctestInfo", "version": {"major": 1}, "tests": []}
        for family in range(4):
            for parameter in range(2):
                name = f"Native.Case{family}/{parameter}"
                self.document["tests"].append({
                    "name": name, "command": [str(self.root / "Owner"),
                        "--gtest_filter=" + name, "--gtest_also_run_disabled_tests"],
                    "properties": [
                        {"name": "LABELS", "value": ["Owner"]},
                        {"name": "ENVIRONMENT", "value": ["NEVERD_SIGNATURE_CACHE=off"]},
                        {"name": "WORKING_DIRECTORY", "value": str(self.root)},
                        {"name": "SKIP_REGULAR_EXPRESSION", "value": [r"\[  SKIPPED \]"]},
                        {"name": "TIMEOUT", "value": 20.0}]})
        ci = SimpleNamespace(hvf_inventory=lambda *_: (["Owner"], {"Native.Case0/0"}))
        patcher = mock.patch.object(diagnostic, "source_modules", return_value=(ci, runner))
        patcher.start()
        self.addCleanup(patcher.stop)
        patcher = mock.patch.object(diagnostic, "source_identity", return_value="a" * 40)
        patcher.start()
        self.addCleanup(patcher.stop)

    def prepare(self, **kwargs):
        evidence = kwargs.pop("evidence", self.build / "evidence")
        with mock.patch.object(diagnostic.subprocess, "check_output", return_value=json.dumps(self.document)):
            return diagnostic.prepare(self.root, self.build, evidence, (0, 2),
                                      kwargs.get("first", 0), kwargs.get("count", 0),
                                      kwargs.get("case_index", -1))

    def test_partial_plan_preserves_original_methods_and_execution_properties(self):
        plan = self.prepare()
        self.assertFalse(plan["complete_inventory"])
        self.assertFalse(plan["executed"])
        self.assertEqual(plan["full_registered"], 8)
        self.assertEqual([m["family"] for m in plan["methods"]], ["Native.Case0", "Native.Case2"])
        expected = runner.shard_inventory(self.document, 0, 2)
        actual = []
        for entry in plan["methods"]:
            directory = self.build / "evidence" / f"method-{entry['index']:04d}"
            actual.extend(json.loads((directory / "inventory.json").read_text())["tests"])
        self.assertEqual(actual, expected["tests"])
        self.assertFalse(list((self.build / "evidence").rglob("results.xml")))

    def test_exact_parameter_selection_preserves_its_ctest_contract(self):
        plan = self.prepare(first=1, count=1, case_index=1)
        self.assertEqual(len(plan["methods"]), 1)
        actual = json.loads((self.build / "evidence/method-0001/inventory.json").read_text())
        self.assertEqual(actual["tests"], [self.document["tests"][5]])

    def test_cmake_cache_comments_and_blank_lines_preserve_configuration(self):
        cache = self.build / "CMakeCache.txt"
        cache.write_text("# generated cache\n\n" + cache.read_text().replace(
            "\n", "\n\n// CMake documentation\n"))
        self.assertEqual(self.prepare()["llvm_prebuilt"], "OFF")

    def test_invalid_ranges_build_or_native_inventory_publish_nothing(self):
        for first, count, case in ((-1, 0, -1), (2, 0, -1), (0, 3, -1),
                                   (0, 1, 2), (0, 0, 0), (0, 1, -2)):
            with self.subTest(first=first, count=count, case=case), self.assertRaises(ValueError):
                self.prepare(first=first, count=count, case_index=case)
            self.assertFalse((self.build / "evidence").exists())
        self.document["tests"] = self.document["tests"][1:]
        with self.assertRaisesRegex(ValueError, "native requirements"):
            self.prepare()
        self.assertFalse((self.build / "evidence").exists())
        (self.build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Debug\n")
        with self.assertRaisesRegex(ValueError, "Release configuration"):
            self.prepare()

    def test_unknown_properties_credentials_and_overwrite_are_rejected(self):
        original = copy.deepcopy(self.document)
        self.document["tests"][0]["properties"].append({"name": "FIXTURES_REQUIRED", "value": ["x"]})
        with self.assertRaises(ValueError):
            self.prepare()
        self.document = original
        self.document["tests"][0]["properties"][1]["value"].append("ACTIONS_RUNTIME_TOKEN=fixture")
        with self.assertRaisesRegex(ValueError, "credentials"):
            self.prepare()
        self.document["tests"][0]["properties"][1]["value"].pop()
        self.prepare()
        before = (self.build / "evidence/plan.json").read_bytes()
        with self.assertRaises(FileExistsError):
            self.prepare()
        self.assertEqual(before, (self.build / "evidence/plan.json").read_bytes())

    def test_changed_source_and_inventory_cannot_execute(self):
        self.prepare()
        evidence = self.build / "evidence"
        with mock.patch.object(diagnostic, "source_identity", return_value="b" * 40):
            with self.assertRaisesRegex(ValueError, "source differs"):
                diagnostic.execute(self.root, evidence, 0)
        path = evidence / "method-0000/inventory.json"
        path.write_text(path.read_text() + " ")
        with self.assertRaisesRegex(ValueError, "inventory changed"):
            diagnostic.execute(self.root, evidence, 0)
        self.assertFalse((evidence / "method-0000/execution").exists())

    def test_runtime_credentials_are_removed_but_tracking_and_test_environment_survive(self):
        environment = {name: "fixture" for name in ("ACTIONS_RUNTIME_TOKEN", "ACTIONS_RESULTS_URL",
            "INPUT_SOURCE", "GITHUB_TOKEN", "GH_TOKEN", "SSH_AUTH_SOCK",
            "RUNNER_TRACKING_ID", "NEVERD_SIGNATURE_CACHE", "PATH")}
        self.assertEqual(set(diagnostic.test_environment(environment)),
                         {"RUNNER_TRACKING_ID", "NEVERD_SIGNATURE_CACHE", "PATH"})

    def test_cancellation_retires_the_independent_native_process_group(self):
        directory = self.build / "cancel"
        directory.mkdir()
        code = ("import subprocess, sys; from pathlib import Path; "
                "from scripts.diagnose_hvf_methods import NativeChildren\n"
                "with NativeChildren(Path(sys.argv[1])):\n"
                " child=subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'], "
                "start_new_session=True)\n child.wait()\n")
        with subprocess.Popen([sys.executable, "-c", code, str(directory)]) as controller:
            self.addCleanup(lambda: controller.poll() is None and controller.kill())
            for _ in range(300):
                if (directory / "children.json").exists():
                    break
                time.sleep(0.01)
            self.assertTrue((directory / "children.json").exists())
            controller.send_signal(signal.SIGTERM)
            self.assertEqual(controller.wait(timeout=10), 128 + signal.SIGTERM)
        retired = json.loads((directory / "retirement.json").read_text())
        self.assertEqual(len(retired), 1)
        self.assertTrue(retired[0]["retired"])
        self.assertEqual(retired[0]["status"], -signal.SIGKILL)
        with self.assertRaises(ProcessLookupError):
            os.kill(retired[0]["pid"], 0)
