"""Retain alternating native/portable digest timings and complete result parity.

Run after builds/tests finish. These primitive timings do not measure CLI, worker
or GUI latency. Hosted-runner contention remains outside the probe's control.
"""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("native", type=Path)
    parser.add_argument("portable", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--pairs", type=int, default=7)
    parser.add_argument("--require-hardware", action="store_true")
    args = parser.parse_args()
    if args.pairs < 1:
        parser.error("--pairs must be positive")
    paths = {"native": args.native.resolve(), "portable": args.portable.resolve()}
    result = {
        "scope": "one-shot SHA-256 only; not whole-request latency",
        "host": platform.uname()._asdict(),
        "binaries": {name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                     for name, path in paths.items()},
        "cases": [],
    }
    for size, iterations in [(0, 10000), (63, 10000), (4096, 16384),
                             (1048576, 64), (67108864, 1)]:
        samples = []
        for pair in range(args.pairs + 1):
            order = ("native", "portable") if pair % 2 == 0 else ("portable", "native")
            sample = {"pair": pair, "warmup": pair == 0, "order": order}
            for name in order:
                run = subprocess.run([str(paths[name]), str(size), str(iterations)],
                                     check=True, capture_output=True, text=True, timeout=120)
                value = json.loads(run.stdout)
                assert value["bytes"] == size and value["iterations"] == iterations
                if name == "portable":
                    assert value["hardware"] is False
                elif args.require_hardware:
                    assert value["hardware"] is True
                sample[name] = value
            for field in ("digest", "consumed"):
                assert sample["native"][field] == sample["portable"][field]
            samples.append(sample)
        summary = {}
        for clock in ("wall_ns", "cpu_ns"):
            times = {name: statistics.median(sample[name][clock] for sample in samples[1:])
                     for name in paths}
            summary[clock] = {**times, "native_over_portable":
                             times["native"] / times["portable"] if times["portable"] else None}
        result["cases"].append({"bytes": size, "iterations": iterations,
                                "summary": summary, "samples": samples})
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"bytes": size, "summary": summary}), flush=True)


if __name__ == "__main__":
    main()
