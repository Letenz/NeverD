#!/usr/bin/env python3
"""Build declared native owners and preserve their complete CTest evidence."""

from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

if __package__:
    from .audit_ci_test_inventory import parse_inventory
    from .audit_ci_test_results import OUTCOME_NAMES, parse_junit
else:
    from audit_ci_test_inventory import parse_inventory
    from audit_ci_test_results import OUTCOME_NAMES, parse_junit


ROOT = Path(__file__).resolve().parents[1]


def read_inventory(
    root: Path, filename: str, prefix: str
) -> tuple[list[str], set[str]]:
    definitions = (root / "scripts" / filename).read_text(encoding="utf-8")
    owners = re.findall(rf"^{prefix}_OWNER\((\w+)\)$", definitions, re.M)
    literal = r'((?:"[^"]+"\s*)+)'
    families = [
        tuple("".join(re.findall(r'"([^"]*)"', field)) for field in fields)
        for fields in re.findall(
            rf'^{prefix}_REQUIRED_CASES\(\s*{literal},\s*{literal},'
            rf'\s*{literal}\)', definitions, re.M
        )
    ]
    explicit = [
        "".join(re.findall(r'"([^"]*)"', value))
        for value in re.findall(
            rf'^{prefix}_REQUIRED_TEST\(\s*((?:"[^"]+"\s*)+)\)',
            definitions, re.M
        )
    ]
    if (not owners or len(set(owners)) != len(owners)
            or not (families or explicit) or len(set(explicit)) != len(explicit)):
        raise ValueError(f"invalid native test inventory: {filename}")
    required = set(explicit)
    for prefix, source, macro in families:
        cases = re.findall(
            rf"^{re.escape(macro)}\(\s*(\w+)\s*,",
            (root / source).read_text(encoding="utf-8"), re.M
        )
        if not cases or len(set(cases)) != len(cases):
            raise ValueError(f"invalid required native case inventory: {source}")
        required.update(prefix + case for case in cases)
    return owners, required


def declared_inventory(
    root: Path, with_drivers: bool = False
) -> tuple[list[str], set[str]]:
    owners, required = read_inventory(root, "NativeCPUTests.def", "NEVERD_NATIVE_CPU")
    if with_drivers:
        driver_owners, driver_required = read_inventory(
            root, "NativeDriverTests.def", "NEVERD_NATIVE_DRIVER"
        )
        if set(owners) & set(driver_owners) or required & driver_required:
            raise ValueError("overlapping native CPU and driver inventories")
        owners += driver_owners
        required |= driver_required
    return owners, required


def run(
    build: Path, evidence: Path, parallel: int, require_whp: bool,
    with_drivers: bool = False,
) -> int:
    owners, required = declared_inventory(ROOT, with_drivers)
    configured = {
        Path(line.replace("\\", "/")).name.removesuffix(".dir")
        for line in (build / "CMakeFiles" / "TargetDirectories.txt")
        .read_text(encoding="utf-8")
        .splitlines()
        if "/unittests/emulation/CMakeFiles/" in line.replace("\\", "/")
    }
    missing = set(owners) - configured
    if missing:
        raise ValueError(f"unconfigured native CPU owners: {sorted(missing)}")
    evidence.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            "cmake", "--build", str(build), "--config", "Release",
            "--target", *owners, "--parallel", str(parallel),
        ],
        check=True,
    )
    labels = "^(" + "|".join(re.escape(owner) for owner in owners) + ")$"
    base = [
        "ctest", "--test-dir", str(build / "unittests" / "emulation"),
        "--build-config", "Release", "-L", labels,
    ]
    inventory = subprocess.check_output([*base, "--show-only=json-v1"], text=True)
    (evidence / "inventory.json").write_text(inventory, encoding="utf-8")
    tests = parse_inventory(json.loads(inventory))
    registered_owners = {label for test in tests for label in test.labels}
    missing_owners = set(owners) - registered_owners
    if missing_owners:
        raise ValueError(f"owners have no registered tests: {sorted(missing_owners)}")
    required_missing = required - {test.name for test in tests}
    if require_whp and required_missing:
        raise ValueError(
            f"missing required native WHP tests: {sorted(required_missing)}"
        )
    junit = evidence / "results.xml"
    result = subprocess.run([
        *base, "--no-tests=error", "--parallel", str(parallel),
        "--output-on-failure", "--output-junit", str(junit),
        "--output-log", str(evidence / "ctest.log"),
    ])
    cases = parse_junit(ET.parse(junit).getroot())
    counts = Counter(case.outcome for case in cases)
    expected, actual = set(tests), {case.test for case in cases}
    required_unexecuted = [
        case.test.name for case in cases
        if case.test.name in required and case.outcome != "passed"
    ]
    summary = {
        "commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        "source_dirty": bool(subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=ROOT, text=True
        ).strip()),
        "owners": owners,
        "registered": len(tests),
        "total": len(cases),
        "counts": {name: counts[name] for name in OUTCOME_NAMES},
        "missing": sorted(test.name for test in expected - actual),
        "unexpected": sorted(test.name for test in actual - expected),
        "require_whp": require_whp,
        "with_drivers": with_drivers,
        "required_native_tests": len(required),
        "required_native_missing": sorted(required_missing),
        "required_native_unexecuted": required_unexecuted,
        "ctest_status": result.returncode,
    }
    (evidence / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary), flush=True)
    return int(bool(
        result.returncode or counts["failed"] or counts["disabled"]
        or counts["not_run"] or expected != actual or len(cases) != len(tests)
        or (require_whp and required_unexecuted)
    ))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--parallel", type=int, default=4)
    parser.add_argument("--require-whp", action="store_true")
    parser.add_argument("--with-drivers", action="store_true")
    args = parser.parse_args()
    if args.parallel < 1:
        parser.error("parallel jobs must be positive")
    return run(
        args.build.resolve(), args.evidence.resolve(), args.parallel,
        args.require_whp, args.with_drivers,
    )


if __name__ == "__main__":
    sys.exit(main())
