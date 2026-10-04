#!/usr/bin/env python3
"""Instrumented, partial evidence for the original one-process HVF recovery loop."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import re
import sys
import subprocess

try:
    from . import diagnose_hvf_methods as shared
except ImportError:
    import diagnose_hvf_methods as shared


def repetition_budget(repetitions):
    if type(repetitions) is not int or repetitions not in (100, 1000):
        raise ValueError("recovery diagnosis requires 100 or 1000 repetitions")
    return 180 if repetitions == 100 else 600


EXPERIMENTS = {
    "lifecycle": "HvfIntelProbe.LifecycleOnly",
    "instruction": "HvfIntelProbe.InstructionOnly",
    "instruction-reuse": "HvfIntelProbe.InstructionOnly",
    "finite-deadline": "HvfIntelProbe.FiniteDeadline",
}


def recovery_contract(source, build, document, required, runner, repetitions, experiment="recovery"):
    if experiment not in ("recovery", *EXPERIMENTS):
        raise ValueError("unknown Intel experiment")
    methods = runner.method_inventory(document)
    selected = [(key, expected) for key, expected in methods.items()
                if (key[1].startswith("HvfExecutor.Native") if experiment == "recovery"
                    else key[1] == EXPERIMENTS[experiment])]
    if len(selected) != 1 or len(selected[0][1]) != 1:
        raise ValueError("selected filter must select exactly one native recovery test")
    key, expected = selected[0]
    name, record = next(iter(expected.items()))
    binary = (build / "bin/NeverDHvfTests").resolve()
    if (Path(key[0]).resolve() != binary or (experiment == "recovery" and record.name not in required)
            or "NeverDHvfTests" not in record.labels):
        raise ValueError("recovery test is not the selected source's required native owner")
    native_filter = "HvfExecutor.Native*" if experiment == "recovery" else EXPERIMENTS[experiment]
    environment = {"NEVERD_REQUIRE_HVF": "1"}
    if experiment != "recovery":
        environment["NEVERD_HVF_INTEL_PROBE"] = "1"
    if experiment == "instruction-reuse":
        environment["NEVERD_HVF_INTEL_REUSE_EXECUTOR"] = "1"
    return {
        "experiment": experiment,
        "native_execution": experiment != "lifecycle",
        "required_for_acceptance": experiment == "recovery",
        "executor_reuse": experiment == "instruction-reuse",
        "native_name": name, "required_ctest_name": record.name,
        "command": [str(binary), "--gtest_filter=" + native_filter,
                    f"--gtest_repeat={repetitions}", "--gtest_break_on_failure"],
        "working_directory": str(source), "repetitions": repetitions,
        "timeout_seconds": repetition_budget(repetitions),
        "native_requirements": environment,
    }


def prepare(source, build, evidence, repetitions, experiment="recovery"):
    repetition_budget(repetitions)
    commit = shared.source_identity(source)
    if platform.system() != "Darwin" or platform.machine() != "x86_64":
        raise ValueError("recovery diagnosis requires a native Intel macOS host")
    cache = shared.build_configuration(source, build)
    if cache.get("NEVERD_LLVM_PREBUILT") != "OFF":
        raise ValueError("Intel recovery diagnosis requires the pinned LLVM source build")
    ci, runner = shared.source_modules(source)
    owners, required = ci.hvf_inventory(source, platform.machine(), True)
    if owners != ["NeverDHvfTests"]:
        raise ValueError("unexpected native transport owner")
    document = json.loads(subprocess.check_output([
        "ctest", "--test-dir", str(build / "unittests/emulation"),
        "--build-config", "Release", "-L", "^NeverDHvfTests$",
        "--show-only=json-v1"], text=True))
    records = runner.parse_inventory(document)
    if required - {record.name for record in records}:
        raise ValueError("transport inventory is missing required native cases")
    contract = recovery_contract(source, build, document, required, runner, repetitions, experiment)
    evidence.mkdir(parents=True, exist_ok=False)
    shared.write_json(evidence / "inventory.json", document)
    plan = {
        "kind": "instrumented-partial-hvf-recovery-plan", "complete_inventory": False,
        "executed": False, "commit": commit, "source_dirty": False,
        "source": str(source), "build": str(build),
        "controller_commit": os.environ.get("GITHUB_SHA"),
        "host_system": platform.system(), "host_architecture": platform.machine(),
        "host_release": platform.release(), "llvm_prebuilt": cache["NEVERD_LLVM_PREBUILT"],
        "inventory_sha256": shared.fingerprint(evidence / "inventory.json"),
        **contract,
    }
    shared.write_json(evidence / "plan.json", plan)
    return plan


def read_repetitions(log, name, repetitions):
    """Require every complete iteration in order; totals or overwritten XML cannot prove this."""
    completed, started, stage = 0, 0, "iteration"
    error = None
    for line in log.splitlines():
        iteration = re.fullmatch(r"Repeating all tests \(iteration ([0-9]+)\) \. \. \.", line)
        run = re.fullmatch(r"\[ RUN      \] (.+)", line)
        passed = re.fullmatch(r"\[       OK \] (.+) \([0-9]+ ms\)", line)
        summary = re.fullmatch(r"\[  PASSED  \] ([0-9]+) tests?\.", line)
        if iteration:
            if stage != "iteration" or int(iteration[1]) != completed + 1 or completed >= repetitions:
                error = "missing, repeated or out-of-order iteration"
                break
            started += 1
            stage = "run"
        elif run:
            if stage != "run" or run[1] != name:
                error = "unexpected or duplicate native RUN"
                break
            stage = "ok"
        elif passed:
            if stage != "ok" or passed[1] != name:
                error = "unexpected or duplicate native OK"
                break
            stage = "summary"
        elif summary:
            if stage != "summary" or summary[1] != "1":
                error = "unexpected or duplicate repetition summary"
                break
            completed += 1
            stage = "iteration"
        elif re.search(r"\[\s*(?:FAILED|SKIPPED)\s*\]|Failure$", line):
            error = "native assertion failure or skip"
            break
    if error is None and (completed != repetitions or stage != "iteration"):
        error = "incomplete repetition evidence"
    return {"started_repetitions": started, "completed_repetitions": completed,
            "next_expected_event": stage, "error": error}


def execute(source, evidence):
    plan = json.loads((evidence / "plan.json").read_text())
    if (plan["source"] != str(source) or plan["commit"] != shared.source_identity(source)
            or plan["complete_inventory"] is not False
            or shared.fingerprint(evidence / "inventory.json") != plan["inventory_sha256"]):
        raise ValueError("recovery source or inventory changed after preparation")
    build = Path(plan["build"])
    cache = shared.build_configuration(source, build)
    if (cache.get("NEVERD_LLVM_PREBUILT") != "OFF" or platform.system() != "Darwin"
            or platform.machine() != "x86_64"):
        raise ValueError("recovery build or host changed after preparation")
    ci, runner = shared.source_modules(source)
    _, required = ci.hvf_inventory(source, platform.machine(), True)
    contract = recovery_contract(source, build,
        json.loads((evidence / "inventory.json").read_text()), required, runner, plan["repetitions"], plan.get("experiment", "recovery"))
    if any(plan.get(key) != value for key, value in contract.items()):
        raise ValueError("recovery command differs from its prepared contract")
    environment = shared.test_environment(os.environ)
    for key in ("NEVERD_HVF_INTEL_PROBE", "NEVERD_HVF_INTEL_REUSE_EXECUTOR"):
        environment.pop(key, None)
    environment.update(contract["native_requirements"])
    with shared.NativeChildren(evidence):
        status = runner.execute(contract["command"], contract["working_directory"],
            environment, contract["timeout_seconds"], evidence / "execution")
    repeats = read_repetitions((evidence / "execution/output.log").read_text(),
                              contract["native_name"], contract["repetitions"])
    retirement = json.loads((evidence / "retirement.json").read_text())
    success = (not repeats["error"] and status["status"] == 0 and not status["timed_out"]
               and status["child_retired"] and len(retirement) == 1
               and retirement[0]["retired"] and retirement[0]["status"] == 0)
    shared.write_json(evidence / "result.json", {
        "kind": "instrumented-partial-hvf-recovery-result", "complete_inventory": False,
        "commit": plan["commit"], "passed": success, "repetitions": repeats,
        "output_sha256": shared.fingerprint(evidence / "execution/output.log"),
        "diagnostic_status": 0 if success else 1,
    })
    return 0 if success else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("prepare", "execute"))
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--experiment", choices=("recovery", *EXPERIMENTS), default="recovery")
    parser.add_argument("--repetitions", type=int, choices=(100, 1000), default=1000)
    args = parser.parse_args()
    if args.mode == "execute":
        return execute(args.source.resolve(), args.evidence.resolve())
    if args.build is None:
        parser.error("prepare requires --build")
    prepare(args.source.resolve(), args.build.resolve(), args.evidence.resolve(), args.repetitions, args.experiment)
    return 0


if __name__ == "__main__":
    sys.exit(main())
