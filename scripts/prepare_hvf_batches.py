#!/usr/bin/env python3
"""Save hosted Intel batch plans before execution can interrupt the runner.

Plans are diagnostics, never acceptance evidence. Every executed batch still
builds and validates the complete native inventory through run_native_cpu_ci.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import platform
import re
import subprocess

if __package__:
    from .run_native_cpu_ci import ROOT, hvf_inventory
    from .run_native_cpu_methods import method_inventory, method_plan, shard_inventory
else:
    from run_native_cpu_ci import ROOT, hvf_inventory
    from run_native_cpu_methods import method_inventory, method_plan, shard_inventory


# The workflow has four native jobs and the composite action has four batches.
JOB_COUNT = 4
BATCHES_PER_JOB = 4
SHARD_COUNT = JOB_COUNT * BATCHES_PER_JOB


def batch_indices(job: int) -> tuple[int, ...]:
    if type(job) is not int or not 0 <= job < JOB_COUNT:
        raise ValueError(f"HVF job must be an integer in [0, {JOB_COUNT})")
    # Keep each job's original INDEX/4 workload, subdividing it into INDEX/16.
    return tuple(range(job, SHARD_COUNT, JOB_COUNT))


def prepare(build: Path, evidence: Path, job: int) -> dict[str, int]:
    indices = batch_indices(job)
    owners, _ = hvf_inventory(ROOT, platform.machine())
    labels = "^(" + "|".join(re.escape(owner) for owner in owners) + ")$"
    raw = subprocess.check_output([
        "ctest", "--test-dir", str(build / "unittests" / "emulation"),
        "--build-config", "Release", "-L", labels, "--show-only=json-v1",
    ], text=True)
    document = json.loads(raw)
    # Validate every selection before publishing any plan. Unknown commands or
    # execution properties must not produce a misleading diagnostic itinerary.
    selected = {index: shard_inventory(document, index, SHARD_COUNT) for index in indices}
    evidence.mkdir(parents=True, exist_ok=False)
    (evidence / "full-inventory.json").write_text(raw, encoding="utf-8")
    for index, inventory in selected.items():
        destination = evidence / f"shard-{index}"
        destination.mkdir()
        (destination / "inventory.json").write_text(
            json.dumps(inventory, indent=2) + "\n", encoding="utf-8")
        (destination / "method-plan.json").write_text(
            json.dumps(method_plan(method_inventory(inventory)), indent=2) + "\n",
            encoding="utf-8")
    (evidence / "plan.json").write_text(json.dumps({
        "kind": "hvf-batch-plan", "executed": False,
        "commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "source_dirty": bool(subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=ROOT, text=True).strip()),
        "host_architecture": platform.machine(), "owners": owners,
        "job": job, "job_count": JOB_COUNT, "shard_count": SHARD_COUNT,
        "shards": list(indices), "registered": len(document["tests"]),
    }, indent=2) + "\n", encoding="utf-8")
    return {"shard_count": SHARD_COUNT,
            **{f"batch_{batch}": index for batch, index in enumerate(indices)}}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--job", type=int, required=True)
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args()
    outputs = prepare(args.build.resolve(), args.evidence.resolve(), args.job)
    if args.github_output:
        with args.github_output.open("a", encoding="utf-8") as output:
            output.writelines(f"{name}={value}\n" for name, value in outputs.items())
    print(json.dumps(outputs))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
