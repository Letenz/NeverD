#!/usr/bin/env python3
"""Temporary, explicitly partial HVF diagnosis using a selected source's runner.

No result from this tool is complete CPU acceptance. The calling Node action
must preserve each prepared marker before permitting its guest execution.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import importlib
import json
import os
from pathlib import Path
import platform
import re
import signal
import subprocess
import sys


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def fingerprint(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_identity(source):
    commit = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    dirty = subprocess.check_output(
        ["git", "status", "--porcelain"], cwd=source, text=True).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", commit) or dirty:
        raise ValueError("diagnostic source must be an exact clean commit")
    return commit


def source_modules(source):
    # Every CLI invocation is a fresh Python process. Use the tested commit's
    # command parser and runner, never a newer controller's interpretation.
    directory = (source / "scripts").resolve()
    sys.path.insert(0, str(directory))
    modules = [importlib.import_module(name) for name in
               ("run_native_cpu_ci", "run_native_cpu_methods")]
    if any(Path(module.__file__).resolve().parent != directory for module in modules):
        raise ValueError("diagnostic imports refer to another source checkout")
    return modules


def select_methods(document, runner, first, count, case_index):
    methods = list(runner.method_inventory(document).items())
    if (type(first) is not int or type(count) is not int or type(case_index) is not int
            or not 0 <= first < len(methods) or count < 0 or case_index < -1
            or (count and first + count > len(methods))
            or (case_index >= 0 and count != 1)):
        raise ValueError("invalid diagnostic method range or case index")
    selected = []
    for index in range(first, first + count if count else len(methods)):
        key, expected = methods[index]
        if case_index >= len(expected):
            raise ValueError("diagnostic case index exceeds the method")
        records = set(expected.values()) if case_index < 0 else {
            list(expected.values())[case_index]}
        inventory = {**document, "tests": [raw for raw, record in zip(
            document["tests"], runner.parse_inventory(document), strict=True)
            if record in records]}
        if len(runner.method_inventory(inventory)) != 1:
            raise ValueError("diagnostic selection changed method ownership")
        selected.append((index, key[1], inventory))
    return selected


def prepare(source, build, evidence, shard, first, count, case_index):
    commit = source_identity(source)
    cache = dict(re.findall(r"^([A-Za-z_][A-Za-z0-9_]*):[^=\r\n]*=(.*)$",
                           (build / "CMakeCache.txt").read_text(), re.M))
    expected = {"CMAKE_HOME_DIRECTORY": str(source), "CMAKE_BUILD_TYPE": "Release",
                "NEVERD_EMULATION_BACKEND_HVF": "ON",
                "NEVERD_EMULATION_BACKEND_UNICORN": "OFF"}
    if any(cache.get(key) != value for key, value in expected.items()):
        raise ValueError("diagnostic build is not the selected source's native Release configuration")
    ci, runner = source_modules(source)
    owners, required = ci.hvf_inventory(source, platform.machine())
    labels = "^(" + "|".join(re.escape(owner) for owner in owners) + ")$"
    document = json.loads(subprocess.check_output([
        "ctest", "--test-dir", str(build / "unittests/emulation"),
        "--build-config", "Release", "-L", labels, "--show-only=json-v1",
    ], text=True))
    records = runner.parse_inventory(document)
    if set(owners) - {label for record in records for label in record.labels}:
        raise ValueError("diagnostic full inventory is missing an owner")
    if required - {record.name for record in records}:
        raise ValueError("diagnostic full inventory is missing native requirements")
    selected = runner.shard_inventory(document, *shard)
    methods = select_methods(selected, runner, first, count, case_index)
    if len(methods) > 200:
        raise ValueError("diagnostic range exceeds the bounded artifact allowance")
    for _, _, inventory in methods:
        for key in runner.method_inventory(inventory):
            variables = dict(item.split("=", 1) for item in key[3])
            if test_environment(variables) != variables:
                raise ValueError("CTest properties reintroduce runner credentials")
    evidence.mkdir(parents=True, exist_ok=False)
    write_json(evidence / "full-inventory.json", document)
    write_json(evidence / "shard-inventory.json", selected)
    plan = {
        "kind": "partial-hvf-method-diagnostic", "complete_inventory": False,
        "executed": False, "commit": commit, "source_dirty": False,
        "controller_commit": os.environ.get("GITHUB_SHA"),
        "llvm_prebuilt": cache.get("NEVERD_LLVM_PREBUILT"),
        "source": str(source), "build": str(build),
        "host_architecture": platform.machine(), "host_system": platform.system(),
        "host_release": platform.release(), "logical_cpus": os.cpu_count(),
        "shard": {"index": shard[0], "count": shard[1]},
        "case_index": case_index, "full_registered": len(records),
        "full_inventory_sha256": fingerprint(evidence / "full-inventory.json"),
        "methods": [], "required_native_names": sorted(required),
    }
    for index, family, inventory in methods:
        destination = evidence / f"method-{index:04d}"
        destination.mkdir()
        write_json(destination / "inventory.json", inventory)
        marker = {
            "kind": "prepared-hvf-method", "executed": False,
            "commit": commit, "index": index, "family": family,
            "case_index": case_index,
            "inventory_sha256": fingerprint(destination / "inventory.json"),
            "method": runner.method_plan(runner.method_inventory(inventory)),
        }
        write_json(destination / "prepared.json", marker)
        plan["methods"].append({"index": index, "family": family,
                                "inventory_sha256": marker["inventory_sha256"]})
    write_json(evidence / "plan.json", plan)
    return plan


def test_environment(environment):
    # Runtime credentials belong only to the artifact uploader. Checkout uses
    # persist-credentials:false so they are not reachable through Git either.
    return {name: value for name, value in environment.items()
            if not name.startswith(("ACTIONS_", "INPUT_"))
            and name not in {"GITHUB_TOKEN", "GH_TOKEN", "SSH_AUTH_SOCK"}}


class NativeChildren:
    """Record the unchanged source runner's children and retire them on cancel."""

    def __init__(self, destination):
        self.destination = destination
        self.children = []
        self.spawning = False
        self.pending_signal = None

    def __enter__(self):
        self.original_popen = subprocess.Popen
        self.handlers = {number: signal.getsignal(number)
                         for number in (signal.SIGINT, signal.SIGTERM)}

        def spawn(*args, **kwargs):
            # Defer cancellation across creation/registration without changing
            # the signal mask inherited by the original test command.
            self.spawning = True
            try:
                child = self.original_popen(*args, **kwargs)
                if kwargs.get("start_new_session"):
                    self.children.append(child)
                    write_json(self.destination / "children.json", {
                        "process_groups": [child.pid for child in self.children]})
                return child
            finally:
                self.spawning = False
                if self.pending_signal is not None:
                    cancel(self.pending_signal, None)

        def cancel(number, _frame):
            if self.spawning:
                self.pending_signal = number
                return
            self.retire()
            raise SystemExit(128 + number)

        subprocess.Popen = spawn
        for number in self.handlers:
            signal.signal(number, cancel)
        return self

    def retire(self):
        results = []
        for child in self.children:
            if child.poll() is None:
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
            results.append({"pid": child.pid, "status": child.poll(),
                            "retired": child.poll() is not None})
        write_json(self.destination / "retirement.json", results)

    def __exit__(self, *_):
        try:
            self.retire()
        finally:
            subprocess.Popen = self.original_popen
            for number, handler in self.handlers.items():
                signal.signal(number, handler)


def execute(source, evidence, index):
    plan = json.loads((evidence / "plan.json").read_text())
    if (plan["source"] != str(source) or plan["commit"] != source_identity(source)
            or plan["complete_inventory"] is not False):
        raise ValueError("diagnostic source differs from its prepared plan")
    entries = [entry for entry in plan["methods"] if entry["index"] == index]
    if len(entries) != 1:
        raise ValueError("diagnostic method is not uniquely planned")
    destination = evidence / f"method-{index:04d}"
    if fingerprint(destination / "inventory.json") != entries[0]["inventory_sha256"]:
        raise ValueError("diagnostic method inventory changed after preparation")
    _, runner = source_modules(source)
    document = json.loads((destination / "inventory.json").read_text())
    marker = json.loads((destination / "prepared.json").read_text())
    methods = runner.method_inventory(document)
    if (len(methods) != 1 or marker["method"] != runner.method_plan(methods)
            or marker["commit"] != plan["commit"] or marker["index"] != index):
        raise ValueError("diagnostic marker differs from the execution contract")
    environment = test_environment(os.environ)
    environment["NEVERD_REQUIRE_HVF"] = "1"
    with NativeChildren(destination):
        outcomes, status = runner.run_methods(document, destination / "execution", environment)
    counts = Counter(case.outcome for case in outcomes)
    required = set(plan["required_native_names"])
    missing = {record.name for record in runner.parse_inventory(document)} - {
        case.test.name for case in outcomes}
    unexecuted = [case.test.name for case in outcomes
                  if case.test.name in required and case.outcome != "passed"]
    failed = bool(status or counts["failed"] or missing or unexecuted)
    write_json(destination / "result.json", {
        "kind": "partial-hvf-method-result", "complete_inventory": False,
        "commit": plan["commit"], "index": index,
        "counts": dict(counts), "missing": sorted(missing),
        "required_native_unexecuted": unexecuted,
        "execution_status": status, "diagnostic_status": int(failed),
    })
    return int(failed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("prepare", "execute"))
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--shard", default="0/16")
    parser.add_argument("--first", type=int, default=0)
    parser.add_argument("--count", type=int, default=0)
    parser.add_argument("--case-index", type=int, default=-1)
    parser.add_argument("--index", type=int)
    args = parser.parse_args()
    source, evidence = args.source.resolve(), args.evidence.resolve()
    if args.mode == "execute":
        if args.index is None or args.index < 0:
            parser.error("execute requires a nonnegative method index")
        return execute(source, evidence, args.index)
    if args.build is None or not re.fullmatch(r"[0-9]+/[0-9]+", args.shard):
        parser.error("prepare requires --build and an INDEX/COUNT shard")
    plan = prepare(source, args.build.resolve(), evidence,
                   tuple(map(int, args.shard.split("/"))), args.first,
                   args.count, args.case_index)
    print(json.dumps({"commit": plan["commit"], "methods": plan["methods"]}), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
