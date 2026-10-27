"""Compare prebuilt decoder adapters; keep timing reports outside the source tree."""

import argparse
import hashlib
import json
import math
import os
import platform
from pathlib import Path
import statistics
import subprocess


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def load():
    return os.getloadavg() if hasattr(os, "getloadavg") else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True,
                        help="JSON recording library SHAs, compiler versions and build flags")
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--minimum-cpu-ms", type=int, default=100)
    args = parser.parse_args()
    if args.samples < 1 or args.minimum_cpu_ms < 1:
        parser.error("samples and minimum-cpu-ms must be positive")
    variants = [("capstone-base", args.baseline, "capstone"),
                ("capstone-candidate", args.candidate, "capstone"),
                ("zydis", args.baseline, "zydis"),
                ("xed", args.baseline, "xed"), ("iced", args.baseline, "iced")]
    binaries = {p.resolve(): digest(p.resolve()) for _, p, _ in variants}
    cases = json.loads(args.manifest.read_text())
    if not cases or len({c["name"] for c in cases}) != len(cases):
        parser.error("manifest requires nonempty, uniquely named cases")
    for case in cases:
        if case["bits"] not in (16, 32, 64):
            parser.error("case bits must be 16, 32 or 64")
        path = (args.manifest.parent / case["path"]).resolve()
        case.update(path=str(path), sha256=digest(path))
    args.out.mkdir(parents=True, exist_ok=True)
    report = {"platform": platform.platform(), "machine": platform.machine(),
              "metadata": json.loads(args.metadata.read_text()), "cases": cases,
              "samples_per_variant": args.samples, "minimum_cpu_ms": args.minimum_cpu_ms,
              "binaries": {str(p): h for p, h in binaries.items()},
              "load_before": load(), "calibration": [], "samples": []}

    def run(variant, case, mode, repeats):
        label, binary, library = variant
        result = subprocess.run([str(binary.resolve()), library, mode, str(case["bits"]),
                                 str(repeats), case["path"]], check=True, capture_output=True,
                                text=True, timeout=120)
        row = json.loads(result.stdout)
        assert row["library"] == library and row["mode"] == mode
        assert row["bits"] == case["bits"] and row["repeats"] == repeats
        return {**row, "case": case["name"], "variant": label, "load": load()}

    for case in cases:
        for mode in ("scan", "format", "detail"):
            probe_repeats = 1
            while True:
                probe = run(variants[0], case, mode, probe_repeats)
                if probe["cpu_ns"] >= 1000000 or probe_repeats == 1000000:
                    break
                probe_repeats *= 10
            repeats = max(1, min(1000000, math.ceil(args.minimum_cpu_ms * 1000000 *
                                                   probe_repeats / max(probe["cpu_ns"], 1))))
            warmup = [run(v, case, mode, 1) for v in variants]
            common = len({(r["instructions"], r["rejected"], r["boundary_hash"])
                          for r in warmup}) == 1
            report["calibration"].append({"case": case["name"], "mode": mode,
                                           "repeats": repeats, "warmup": warmup})
            for sample in range(args.samples):
                rotation = sample % len(variants)
                order = variants[rotation:] + variants[:rotation]
                if sample % 2:
                    order.reverse()
                rows = [run(v, case, mode, repeats) for v in order]
                pair = {r["variant"]: r for r in rows}
                for key in ("instructions", "rejected", "boundary_hash", "value_hash"):
                    assert pair["capstone-base"][key] == pair["capstone-candidate"][key], (
                        case["name"], mode, key, "Capstone traversal or consumed values changed")
                for row in rows:
                    row.update(sample=sample, common_traversal=common)
                report["samples"].extend(rows)
                (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
            rows = [r for r in report["samples"] if r["case"] == case["name"] and r["mode"] == mode]
            medians = {v[0]: statistics.median(r["cpu_ns"] / max(r["instructions"], 1)
                                               for r in rows if r["variant"] == v[0]) for v in variants}
            print(case["name"], mode, "common boundaries" if common else "different acceptance",
                  json.dumps(medians), flush=True)
    for path, expected in binaries.items():
        assert digest(path) == expected, (path, "binary changed during measurement")
    for case in cases:
        assert digest(Path(case["path"])) == case["sha256"], (case["name"], "input changed")
    report["load_after"] = load()
    (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
