import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from scripts import prepare_hvf_batches as batches
from scripts.run_native_cpu_methods import parse_inventory, shard_inventory


class HVFBatchPlanTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        # Uneven families, multiple owners and different per-parameter policies
        # must still cover every identity once and preserve each old job's set.
        self.document = {"kind": "ctestInfo", "version": {"major": 1}, "tests": []}
        for family in range(53):
            owner = "OwnerA" if family % 2 else "OwnerB"
            for parameter in range(2):
                name = f"Native.Case{family}/{parameter}"
                self.document["tests"].append({
                    "name": name, "command": [str(self.root / owner),
                        "--gtest_filter=" + name, "--gtest_also_run_disabled_tests"],
                    "properties": [
                        {"name": "LABELS", "value": [owner]},
                        {"name": "ENVIRONMENT", "value": [f"CASE_VALUE={parameter}"]},
                        {"name": "WORKING_DIRECTORY", "value": str(self.root)},
                        {"name": "SKIP_REGULAR_EXPRESSION", "value": [r"\[  SKIPPED \]"]},
                        {"name": "TIMEOUT", "value": 20.0 + parameter}]})

    def prepare(self, job, evidence):
        def capture(command, **kwargs):
            if command[0] == "git":
                return "a" * 40 if command[1] == "rev-parse" else ""
            self.assertEqual(command[0], "ctest")
            self.assertIn("--show-only=json-v1", command)
            self.assertEqual(command[command.index("-L") + 1], "^(OwnerA|OwnerB)$")
            return json.dumps(self.document)

        with mock.patch.object(batches, "hvf_inventory", return_value=(["OwnerA", "OwnerB"], set())), \
                mock.patch.object(batches.subprocess, "check_output", side_effect=capture), \
                mock.patch.object(batches.subprocess, "run") as execute:
            result = batches.prepare(self.root / "build", evidence, job)
            execute.assert_not_called()
            return result

    def test_plans_partition_all_identities_and_preserve_each_original_job(self):
        seen, indices = set(), set()
        for job in range(batches.JOB_COUNT):
            evidence = self.root / f"job-{job}"
            outputs = self.prepare(job, evidence)
            plan = json.loads((evidence / "plan.json").read_text())
            self.assertFalse(plan["executed"])
            self.assertNotIn("passed", plan)
            self.assertFalse(list(evidence.rglob("summary.json")))
            self.assertEqual(plan["commit"], "a" * 40)
            self.assertFalse(plan["source_dirty"])
            self.assertEqual(outputs["shard_count"], batches.SHARD_COUNT)
            job_records = set()
            for batch in range(batches.BATCHES_PER_JOB):
                index = outputs[f"batch_{batch}"]
                self.assertNotIn(index, indices)
                indices.add(index)
                selected = json.loads((evidence / f"shard-{index}/inventory.json").read_text())
                records = set(parse_inventory(selected))
                self.assertFalse(seen & records)
                seen.update(records)
                job_records.update(records)
                methods = json.loads((evidence / f"shard-{index}/method-plan.json").read_text())
                self.assertEqual({name for method in methods for name in method["identities"]},
                                 {test["name"] for test in selected["tests"]})
                # Parameters with different execution policies remain on the same shard.
                names = {test["name"] for test in selected["tests"]}
                self.assertTrue(all(name[:-1] + str(1 - int(name[-1])) in names for name in names))
            self.assertEqual(job_records, set(parse_inventory(
                shard_inventory(self.document, job, batches.JOB_COUNT))))
        self.assertEqual(seen, set(parse_inventory(self.document)))
        self.assertEqual(indices, set(range(batches.SHARD_COUNT)))

    def test_invalid_job_or_execution_contract_cannot_publish_partial_plans(self):
        for job in (-1, batches.JOB_COUNT, True, 1.5):
            evidence = self.root / f"invalid-{job}"
            with self.subTest(job=job), self.assertRaises(ValueError):
                self.prepare(job, evidence)
            self.assertFalse(evidence.exists())
        original = copy.deepcopy(self.document)
        self.document["tests"][-1]["properties"].append({"name": "FIXTURES_REQUIRED", "value": ["f"]})
        evidence = self.root / "unknown-contract"
        with self.assertRaisesRegex(ValueError, "unsupported CTest properties"):
            self.prepare(0, evidence)
        self.assertFalse(evidence.exists())
        self.document = original

    def test_previous_plans_are_not_overwritten(self):
        evidence = self.root / "previous"
        self.prepare(0, evidence)
        before = {path: path.read_bytes() for path in evidence.rglob("*") if path.is_file()}
        with self.assertRaises(FileExistsError):
            self.prepare(1, evidence)
        self.assertEqual(before, {path: path.read_bytes() for path in evidence.rglob("*") if path.is_file()})
