#!/usr/bin/env python3
"""Reconcile complete native HVF coverage from original method-shard evidence."""

from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

if __package__:
    from .audit_ci_test_inventory import parse_inventory
    from .audit_ci_test_results import OUTCOME_NAMES
    from .run_native_cpu_ci import ROOT, hvf_inventory
    from .run_native_cpu_methods import inventory_contract, method_inventory, read_results, shard_inventory
else:
    from audit_ci_test_inventory import parse_inventory
    from audit_ci_test_results import OUTCOME_NAMES
    from run_native_cpu_ci import ROOT, hvf_inventory
    from run_native_cpu_methods import inventory_contract, method_inventory, read_results, shard_inventory


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def host_isa(value):
    return {"arm64": "arm64", "aarch64": "arm64",
            "x86_64": "x86_64", "amd64": "x86_64"}.get(value.lower())


def audit_shards(paths, commit, architecture, count, root=ROOT):
    require(bool(re.fullmatch(r"[0-9a-f]{40}", commit)), "expected a full source commit")
    require(type(count) is int and count >= 2, "expected at least two shards")
    require(len(paths) == count, "missing or extra shard evidence directories")
    architecture = host_isa(architecture)
    require(architecture is not None, "HVF requires an ARM64 or x64 host")
    owners, required = hvf_inventory(root, architecture)
    reference, reference_contract, combined, indexes, hosts = None, None, {}, set(), []
    method_count = 0
    for path in paths:
        summary = read_json(path / "summary.json")
        shard = summary["hvf_shard"]
        require(isinstance(shard, dict) and type(shard.get("index")) is int
                and type(shard.get("count")) is int and shard["count"] == count
                and 0 <= shard["index"] < count, "invalid shard identity")
        index = shard["index"]
        require(index not in indexes, "duplicate shard identity")
        indexes.add(index)
        require(summary["commit"] == commit and summary["source_dirty"] is False,
                "shards must use the expected clean source commit")
        require(host_isa(summary["host_architecture"]) == architecture
                and summary["host_platform"]["system"] == "Darwin",
                "shard did not run on the required native macOS ISA")
        require(summary["require_hvf"] is True and summary["hvf_transport_only"] is False
                and summary["require_whp"] is False and summary["with_drivers"] is False
                and summary["darwin_backend"] is None and summary["execution"] == "gtest-methods"
                and summary["parallel"] == 1, "shard is not complete-profile native HVF method evidence")
        require(summary["owners"] == owners, "shard owners differ from the current full inventory")
        full = read_json(path / "full-inventory.json")
        full_records = parse_inventory(full)
        require(len(full_records) == len(set(full_records)), "duplicate full inventory identity")
        require({owner for record in full_records for owner in record.labels if owner in owners}
                == set(owners), "full inventory is missing an owner")
        require(all(len(record.labels & set(owners)) == 1 for record in full_records),
                "full inventory has an unknown or ambiguous owner")
        require(required <= {record.name for record in full_records},
                "full inventory is missing a required native test")
        if reference is None:
            reference = set(full_records)
            reference_contract = inventory_contract(full)
        require(set(full_records) == reference, "shards disagree on the complete inventory")
        require(inventory_contract(full) == reference_contract,
                "shards disagree on full inventory execution contracts")
        selected = shard_inventory(full, index, count)
        actual_inventory = read_json(path / "inventory.json")
        require(method_inventory(actual_inventory) == method_inventory(selected),
                "selected inventory differs from the declared complete-method partition")
        selected_records = parse_inventory(selected)
        selected_required = required & {record.name for record in selected_records}
        cases = []
        methods = method_inventory(selected)
        plan = [{"index": number, "binary": key[0], "family": key[1], "identities": {
            name: {"name": record.name, "labels": sorted(record.labels)}
            for name, record in expected.items()}}
            for number, (key, expected) in enumerate(methods.items())]
        require(read_json(path / "method-plan.json") == plan,
                "method plan differs from the selected inventory")
        method_paths = {f"{number:04d}" for number in range(len(methods))}
        require({item.name for item in (path / "methods").iterdir()} == method_paths,
                "missing or extra method evidence")
        for number, (key, expected) in enumerate(methods.items()):
            binary, family, directory, variables, timeout, _run_serial = key
            part = path / "methods" / f"{number:04d}"
            status = read_json(part / "status.json")
            command = status["command"]
            require(len(command) == 4 and command[:3] == [
                binary, "--gtest_filter=" + ":".join(expected), "--gtest_also_run_disabled_tests"]
                and command[3].startswith("--gtest_output=xml:")
                and Path(command[3].removeprefix("--gtest_output=xml:")).is_absolute()
                and Path(command[3].removeprefix("--gtest_output=xml:")).parts[-3:]
                == ("methods", f"{number:04d}", "results.xml"),
                "method command differs from the CTest contract: " + family)
            require(status["working_directory"] == directory
                    and status["timeout_seconds"] == min(120, timeout * len(expected))
                    and status["native_requirements"].get("NEVERD_REQUIRE_HVF") == "1"
                    and all(status["test_environment"].get(name) == value
                            for name, value in (item.split("=", 1) for item in variables)
                            if name == "NEVERD_SIGNATURE_CACHE"),
                    "method execution policy differs: " + family)
            require(type(status["status"]) is int and status["status"] == 0
                    and status["timed_out"] is False and status["child_retired"] is True,
                    "method process failed or did not finish: " + family)
            identities = {name: {"name": record.name, "labels": sorted(record.labels)}
                          for name, record in expected.items()}
            require(read_json(part / "identities.json") == identities,
                    "method identity mapping differs: " + family)
            cases.extend(read_results(part / "results.xml", expected))
        counts = Counter(case.outcome for case in cases)
        require(not any(counts[name] for name in ("failed", "disabled", "not_run")),
                "shard contains failed or unexecuted tests")
        require(all(case.outcome == "passed" for case in cases if case.test.name in required),
                "required native coverage was skipped")
        expected_summary = {
            "full_registered": len(full_records), "registered": len(selected_records),
            "total": len(cases), "counts": {name: counts[name] for name in OUTCOME_NAMES},
            "missing": [], "unexpected": [], "execution_status": 0, "ctest_status": None,
            "required_native_tests": len(selected_required),
            "required_native_names": sorted(selected_required),
            "required_native_missing": [], "required_native_unexecuted": [],
        }
        require(all(summary.get(key) == value for key, value in expected_summary.items()),
                "shard summary disagrees with original evidence")
        expected_report = {"execution": "gtest-methods", "errors": [], "results": [
            {"name": case.test.name, "labels": sorted(case.test.labels),
             "outcome": case.outcome, "message": case.message} for case in cases]}
        require(read_json(path / "method-results.json") == expected_report,
                "method report disagrees with original XML")
        require(len(cases) == len(selected_records)
                and {case.test for case in cases} == set(selected_records),
                "shard result coverage is incomplete")
        for case in cases:
            require(case.test not in combined, "overlapping shard results: " + case.test.name)
            combined[case.test] = case.outcome
        method_count += len(methods)
        hosts.append({"index": index, "platform": summary["host_platform"],
                      "logical_cpus": summary["logical_cpus"]})
    require(indexes == set(range(count)) and set(combined) == reference,
            "shard union does not equal the complete CTest inventory")
    counts = Counter(combined.values())
    return {"commit": commit, "source_dirty": False, "host_architecture": architecture,
            "execution": "gtest-method-shards", "shards": count, "owners": owners,
            "registered": len(reference), "total": len(combined), "methods": method_count,
            "counts": {name: counts[name] for name in OUTCOME_NAMES},
            "required_native_names": sorted(required), "required_native_tests": len(required),
            "hosts": sorted(hosts, key=lambda host: host["index"]), "passed": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-root", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--host", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--shards", type=int, default=4)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        paths = sorted(path.parent for path in args.evidence_root.glob("*/summary.json"))
        result = audit_shards(paths, args.commit, args.host, args.shards)
    except (OSError, ValueError, KeyError, TypeError, AttributeError, ET.ParseError) as error:
        result = {"passed": False, "error": str(error)}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result), flush=True)
    return int(not result["passed"])


if __name__ == "__main__":
    sys.exit(main())
