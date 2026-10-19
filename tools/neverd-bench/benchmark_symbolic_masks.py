#!/usr/bin/env python3
"""Compare two builds on independently generated full-width MBA identities.

Keep each CLI beside its own shared SDK library. The benchmark pins library
lookup to that directory; copying only the executable would compare the same
engine twice. Timing includes parsing and proof, excludes process startup, and
is descriptive rather than a machine-dependent pass/fail threshold.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def engine(path):
    path = Path(path).resolve()
    libraries = [
        p for p in path.parent.glob("*neverd*") if p.suffix in (".dylib", ".so", ".dll")
    ]
    if not libraries:
        raise ValueError(f"Put the matching shared SDK beside {path}")
    return {
        "path": str(path),
        "sha256": digest(path),
        "libraries": {p.name: digest(p) for p in libraries},
    }


def corpus(width, count):
    rng = random.Random(0x4D424100 + width)
    full = (1 << width) - 1
    rows = []
    for index in range(count):
        a, b, k, tail = (rng.getrandbits(width) for _ in range(4))
        k |= 1
        # Both complements and deliberately noncomplementary neighbours occur.
        opposite = full ^ a ^ (index % 4 == 3)
        right = "y" if index % 2 == 0 else f"(y^{b})"
        rows.append(f"({k}*((((x^{a})|{right})+(x^{opposite})))+{k})|{tail}")
    return rows


def run(engine_info, corpus_path, width):
    executable = engine_info["path"]
    environment = os.environ.copy()
    variable = (
        "DYLD_LIBRARY_PATH" if platform.system() == "Darwin" else "LD_LIBRARY_PATH"
    )
    environment[variable] = str(Path(executable).parent)
    command = [
        executable,
        "simplify",
        "--json",
        "--stats",
        f"--width={width}",
        "--max-work=65536",
        "-f",
        str(corpus_path),
    ]
    result = subprocess.run(
        command, env=environment, capture_output=True, text=True, timeout=120
    )
    # A bounded search may legitimately decline. Malformed queries may not.
    report = json.loads(result.stdout)
    if any(not row["ok"] for row in report["results"]):
        raise RuntimeError(result.stderr or result.stdout)
    return {"returncode": result.returncode, **report}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=7)
    parser.add_argument("--count", type=int, default=32)
    args = parser.parse_args()
    if args.rounds < 1 or args.count < 1:
        parser.error("rounds and count must be positive")
    engines = {"baseline": engine(args.baseline), "candidate": engine(args.candidate)}
    report = {
        "schemaVersion": 1,
        "platform": platform.platform(),
        "engines": engines,
        "rounds": args.rounds,
        "widths": [],
    }
    with tempfile.TemporaryDirectory(prefix="neverd-mask-bench-") as directory:
        path = Path(directory) / "expressions.txt"
        for width in (8, 16, 32, 64):
            expressions = corpus(width, args.count)
            path.write_text("\n".join(expressions) + "\n", encoding="utf-8")
            row = {
                "width": width,
                "corpusSha256": digest(path),
                "expressions": expressions,
                "runs": {name: [] for name in engines},
            }
            # Warm both processes, then alternate order to reduce thermal/load bias.
            for info in engines.values():
                run(info, path, width)
            for iteration in range(args.rounds):
                for name in (
                    list(engines) if iteration % 2 == 0 else list(reversed(engines))
                ):
                    row["runs"][name].append(run(engines[name], path, width))
            row["medians"] = {
                name: {
                    key: statistics.median(r["summary"][key] for r in runs)
                    for key in (
                        "p50Micros",
                        "p95Micros",
                        "work",
                        "costAfter",
                        "simplified",
                    )
                }
                for name, runs in row["runs"].items()
            }
            report["widths"].append(row)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(
                json.dumps(report, indent=2) + "\n", encoding="utf-8"
            )
            print(width, json.dumps(row["medians"]), flush=True)


if __name__ == "__main__":
    main()
