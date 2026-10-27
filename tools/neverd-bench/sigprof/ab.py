#!/usr/bin/env python3
"""Alternate two NeverD commands and compare their CPU and wall time.

    python3 tools/neverd-bench/sigprof/ab.py --runs 7 \
      --a 'build-a/bin/neverd' --b 'NAME=VALUE build-b/bin/neverd' \
      -- decompile IMAGE --max-func=250 -o '{out}'

Each side is optional NAME=VALUE environment assignments followed by the
binary. '{out}' in the arguments becomes a per-side output file; the hashes
of every run's output are printed and must agree before any timing counts.
Runs alternate ABBA so that host load drifts affect both sides alike.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import resource
import shlex
import statistics
import subprocess
import tempfile
import time


def run(side: str, arguments: list[str], output: str):
    words = shlex.split(side)
    environment = dict(os.environ)
    while words and "=" in words[0] and not words[0].startswith(("/", ".")):
        name, value = words.pop(0).split("=", 1)
        environment[name] = value
    command = words + [arg.replace("{out}", output) for arg in arguments]
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    started = time.monotonic()
    code = subprocess.run(command, env=environment, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode
    wall = time.monotonic() - started
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)
    digest = "-"
    if os.path.exists(output):
        with open(output, "rb") as produced:
            digest = hashlib.sha256(produced.read()).hexdigest()[:16]
        os.remove(output)
    return code, cpu, wall, digest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--a", required=True)
    parser.add_argument("--b", required=True)
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    arguments = args.arguments[1:] if args.arguments[:1] == ["--"] else args.arguments
    scratch = tempfile.mkdtemp(dir=os.environ.get("TMPDIR"))
    results = {"A": [], "B": []}
    for index in range(args.runs):
        order = ("A", "B") if index % 2 == 0 else ("B", "A")
        for name in order:
            side = args.a if name == "A" else args.b
            results[name].append(run(side, arguments,
                                     os.path.join(scratch, name + ".out")))
    for name, rows in results.items():
        print(f"{name}: exit {sorted({row[0] for row in rows})} "
              f"hash {sorted({row[3] for row in rows})} "
              f"cpu {statistics.median(row[1] for row in rows):.2f}s "
              f"wall {statistics.median(row[2] for row in rows):.2f}s (medians)")
    for column, label in ((1, "cpu"), (2, "wall")):
        ratios = [b[column] / a[column] - 1
                  for a, b in zip(results["A"], results["B"])]
        print(f"B/A {label}: median {statistics.median(ratios):+.1%}, "
              f"range [{min(ratios):+.1%}, {max(ratios):+.1%}]")


if __name__ == "__main__":
    main()
