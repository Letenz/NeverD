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

from scripts import diagnose_hvf_recovery as diagnostic
from scripts import run_native_cpu_methods as runner


NAME = "HvfExecutor.NativeIntelCancellationAndCompletionFailureAllowRetry"


def iteration(number, name=NAME):
    return (f"Repeating all tests (iteration {number}) . . .\n"
            f"[ RUN      ] {name}\n[       OK ] {name} (150 ms)\n"
            "[  PASSED  ] 1 test.\n")


class RecoveryLogTests(unittest.TestCase):
    def test_requires_continuous_exact_native_iterations(self):
        log = "".join(iteration(i) for i in range(1, 101))
        self.assertIsNone(diagnostic.read_repetitions(log, NAME, 100)["error"])
        mutations = [log.replace("iteration 31", "iteration 30"),
                     log.replace(iteration(22), ""), log + iteration(101),
                     log.replace(NAME, "HvfExecutor.Other", 1),
                     log.replace("[  PASSED  ] 1 test.", "[  PASSED  ] 2 tests.", 1),
                     log.replace("[ RUN      ]", "[  SKIPPED ]", 1),
                     log.replace(f"[ RUN      ] {NAME}", f"[ RUN      ] {NAME}\n[ RUN      ] {NAME}", 1),
                     log + "[  FAILED  ] native\n", log.rsplit("[  PASSED  ]", 1)[0]]
        for malformed in mutations:
            with self.subTest(log=malformed[-160:]):
                self.assertIsNotNone(diagnostic.read_repetitions(malformed, NAME, 100)["error"])

    def test_green_previous_round_is_not_evidence_of_a_later_failure(self):
        log = iteration(1) + f"Repeating all tests (iteration 2) . . .\n[ RUN      ] {NAME}\nsource:1: Failure\n"
        result = diagnostic.read_repetitions(log, NAME, 100)
        self.assertEqual(result["completed_repetitions"], 1)
        self.assertEqual(result["started_repetitions"], 2)
        self.assertIn("failure", result["error"])


class RecoveryContractTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.build = self.root / "build"
        (self.build / "bin").mkdir(parents=True)
        self.binary = self.build / "bin/NeverDHvfTests"
        self.evidence = self.build / "evidence"
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root}\nCMAKE_BUILD_TYPE:STRING=Release\n"
            "NEVERD_EMULATION_BACKEND_HVF:BOOL=ON\nNEVERD_EMULATION_BACKEND_UNICORN:BOOL=OFF\n"
            "NEVERD_LLVM_PREBUILT:BOOL=OFF\n")
        self.document = {"kind": "ctestInfo", "version": {"major": 1}, "tests": [{
            "name": NAME, "command": [str(self.binary), "--gtest_filter=" + NAME,
                                     "--gtest_also_run_disabled_tests"],
            "properties": [
                {"name": "LABELS", "value": ["NeverDHvfTests"]},
                {"name": "ENVIRONMENT", "value": ["NEVERD_SIGNATURE_CACHE=off"]},
                {"name": "WORKING_DIRECTORY", "value": str(self.root)},
                {"name": "SKIP_REGULAR_EXPRESSION", "value": [r"\[  SKIPPED \]"]},
                {"name": "TIMEOUT", "value": 20.0}]}]}
        ci = SimpleNamespace(hvf_inventory=lambda *_: (["NeverDHvfTests"], {NAME}))
        self.patches = [
            mock.patch.object(diagnostic.shared, "source_identity", return_value="a" * 40),
            mock.patch.object(diagnostic.shared, "source_modules", return_value=(ci, runner)),
            mock.patch.object(diagnostic.platform, "machine", return_value="x86_64"),
            mock.patch.object(diagnostic.platform, "system", return_value="Darwin")]
        for patch in self.patches:
            patch.start()
            self.addCleanup(patch.stop)

    def prepare(self, repetitions=100):
        with mock.patch.object(diagnostic.subprocess, "check_output", return_value=json.dumps(self.document)):
            return diagnostic.prepare(self.root, self.build, self.evidence, repetitions)

    def executable(self, suffix=""):
        log = "".join(iteration(i) for i in range(1, 101))
        self.binary.write_text(f"#!{sys.executable}\nimport os, sys, signal\n"
            "assert os.environ['NEVERD_REQUIRE_HVF'] == '1'\n"
            "assert 'ACTIONS_RUNTIME_TOKEN' not in os.environ\n"
            "assert 'GITHUB_TOKEN' not in os.environ\n"
            "assert sys.argv[1:] == ['--gtest_filter=HvfExecutor.Native*', '--gtest_repeat=100', '--gtest_break_on_failure']\n"
            f"assert os.getcwd() == {str(self.root)!r}\nprint({log!r}, end='', flush=True)\n{suffix}\n")
        self.binary.chmod(0o755)

    def test_original_single_process_argv_and_required_native_environment(self):
        plan = self.prepare()
        self.assertEqual(plan["timeout_seconds"], 180)
        self.assertFalse(plan["complete_inventory"])
        self.assertEqual(len(plan["command"]), 4)
        self.executable()
        with mock.patch.dict(os.environ, {"ACTIONS_RUNTIME_TOKEN": "secret", "GITHUB_TOKEN": "secret"}):
            self.assertEqual(diagnostic.execute(self.root, self.evidence), 0)
        self.assertTrue(json.loads((self.evidence / "result.json").read_text())["passed"])
        self.assertEqual(len(json.loads((self.evidence / "retirement.json").read_text())), 1)
        self.assertFalse(list(self.evidence.rglob("*.xml")))

    def test_all_ok_then_abnormal_exit_is_failure(self):
        self.prepare()
        self.executable("sys.exit(3)")
        self.assertEqual(diagnostic.execute(self.root, self.evidence), 1)
        result = json.loads((self.evidence / "result.json").read_text())
        self.assertEqual(result["repetitions"]["completed_repetitions"], 100)
        self.assertFalse(result["passed"])

    def test_configuration_and_wrong_architecture_fail_before_execution(self):
        with mock.patch.object(diagnostic.platform, "machine", return_value="arm64"):
            with self.assertRaisesRegex(ValueError, "Intel"):
                self.prepare()
        cache = self.build / "CMakeCache.txt"
        original = cache.read_text()
        for before, after in [("Release", "Debug"), ("HVF:BOOL=ON", "HVF:BOOL=OFF"),
                              ("UNICORN:BOOL=OFF", "UNICORN:BOOL=ON"),
                              ("PREBUILT:BOOL=OFF", "PREBUILT:BOOL=ON"),
                              (str(self.root), "/different/source")]:
            cache.write_text(original.replace(before, after))
            with self.assertRaises(ValueError):
                self.prepare()
        cache.write_text(original)
        for count in (0, 1, 101, True):
            with self.assertRaises(ValueError):
                self.prepare(count)
        self.assertFalse(self.evidence.exists())

    def test_missing_ambiguous_or_wrong_native_identity_rejected(self):
        original = copy.deepcopy(self.document)
        self.document["tests"] = []
        with self.assertRaisesRegex(ValueError, "zero tests"):
            self.prepare()
        self.document = copy.deepcopy(original)
        extra = copy.deepcopy(self.document["tests"][0])
        extra["name"] += "Other"
        extra["command"][1] += "Other"
        self.document["tests"].append(extra)
        with self.assertRaisesRegex(ValueError, "exactly one"):
            self.prepare()
        self.document = copy.deepcopy(original)
        self.document["tests"][0]["command"][0] = str(self.root / "different-binary")
        with self.assertRaisesRegex(ValueError, "owner"):
            self.prepare()

    def test_changed_source_or_prepared_command_cannot_execute(self):
        self.prepare()
        with mock.patch.object(diagnostic.shared, "source_identity", return_value="b" * 40):
            with self.assertRaisesRegex(ValueError, "changed"):
                diagnostic.execute(self.root, self.evidence)
        with mock.patch.object(diagnostic.shared, "source_identity", side_effect=ValueError("dirty source")):
            with self.assertRaisesRegex(ValueError, "dirty"):
                diagnostic.execute(self.root, self.evidence)
        plan = json.loads((self.evidence / "plan.json").read_text())
        plan["command"].append("--gtest_filter=*")
        diagnostic.shared.write_json(self.evidence / "plan.json", plan)
        with self.assertRaisesRegex(ValueError, "contract"):
            diagnostic.execute(self.root, self.evidence)
        self.assertFalse((self.evidence / "children.json").exists())

    def test_actual_detached_child_is_retired_on_timeout(self):
        with diagnostic.shared.NativeChildren(self.evidence_parent()):
            status = runner.execute([sys.executable, "-c", "import time; time.sleep(60)"],
                str(self.root), {}, 0.1, self.evidence / "execution")
        self.assertTrue(status["timed_out"])
        self.assertTrue(status["child_retired"])
        self.assertEqual(status["status"], -signal.SIGKILL)
        self.assertTrue(json.loads((self.evidence / "retirement.json").read_text())[0]["retired"])

    def evidence_parent(self):
        self.evidence.mkdir()
        return self.evidence

    def test_actual_controller_cancellation_retires_native_group(self):
        self.evidence_parent()
        program = (
            "import os, sys\nfrom pathlib import Path\n"
            "from scripts.diagnose_hvf_methods import NativeChildren\n"
            "from scripts.run_native_cpu_methods import execute\n"
            "p = Path(sys.argv[1])\nwith NativeChildren(p):\n"
            " execute([sys.executable, '-c', 'import time; time.sleep(60)'], str(p), {}, 60, p / 'execution')\n")
        child = subprocess.Popen([sys.executable, "-c", program, str(self.evidence)])
        try:
            deadline = time.monotonic() + 5
            while not (self.evidence / "children.json").exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue((self.evidence / "children.json").exists())
            child.terminate()
            self.assertEqual(child.wait(timeout=10), 128 + signal.SIGTERM)
            retirement = json.loads((self.evidence / "retirement.json").read_text())
            self.assertTrue(retirement[0]["retired"])
            with self.assertRaises(ProcessLookupError):
                os.killpg(retirement[0]["pid"], 0)
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()


if __name__ == "__main__":
    unittest.main()
