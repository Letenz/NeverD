#!/usr/bin/env python3
"""Build declared native owners and preserve their complete CTest evidence."""

from __future__ import annotations

import argparse
from collections import Counter
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

if __package__:
    from .audit_ci_test_inventory import parse_inventory
    from .audit_ci_test_results import OUTCOME_NAMES, parse_junit
    from .run_native_cpu_methods import run_methods, shard_inventory
else:
    from audit_ci_test_inventory import parse_inventory
    from audit_ci_test_results import OUTCOME_NAMES, parse_junit
    from run_native_cpu_methods import run_methods, shard_inventory


ROOT = Path(__file__).resolve().parents[1]


def read_inventory(
    root: Path, filename: str, prefix: str, host_architecture: str | None = None,
    backend: str | None = None,
) -> tuple[list[str], set[str]]:
    definitions = (root / "scripts" / filename).read_text(encoding="utf-8")
    if backend is not None:
        if backend not in {"kvm", "whp"}:
            raise ValueError("native CPU coverage requires KVM or WHP")
        prefix += rf"(?:_{backend.upper()})?"
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
    host_tests = [
        (architecture, "".join(re.findall(r'"([^"]*)"', value)))
        for architecture, value in re.findall(
            rf'^{prefix}_REQUIRED_HOST_TEST\(\s*(\w+),\s*{literal}\)',
            definitions, re.M,
        )
    ]
    if (not owners or len(set(owners)) != len(owners)
            or not (families or explicit or host_tests)
            or len(set(explicit)) != len(explicit)
            or len(set(host_tests)) != len(host_tests)
            or any(architecture not in {"ARM64", "X64"} for architecture, _ in host_tests)):
        raise ValueError(f"invalid native test inventory: {filename}")
    required = set(explicit)
    if host_tests:
        architecture = {"arm64": "ARM64", "aarch64": "ARM64", "x86_64": "X64", "amd64": "X64"}.get(
            (host_architecture or "").lower()
        )
        if architecture is None:
            raise ValueError("host-specific native tests require an ARM64 or x64 host architecture")
        required.update(name for host, name in host_tests if host == architecture)
    for prefix, source, macro in families:
        cases = re.findall(
            rf"^{re.escape(macro)}\(\s*(\w+)\s*,",
            (root / source).read_text(encoding="utf-8"), re.M
        )
        if not cases or len(set(cases)) != len(cases):
            raise ValueError(f"invalid required native case inventory: {source}")
        required.update(prefix + case for case in cases)
    if not required:
        raise ValueError(f"native test inventory has no required cases for this host: {filename}")
    return owners, required


def declared_inventory(
    root: Path, with_drivers: bool = False, backend: str = "whp",
) -> tuple[list[str], set[str]]:
    owners, required = read_inventory(
        root, "NativeCPUTests.def", "NEVERD_NATIVE_CPU", backend=backend,
    )
    if with_drivers:
        driver_owners, driver_required = read_inventory(
            root, "NativeDriverTests.def", "NEVERD_NATIVE_DRIVER", backend=backend,
        )
        if set(owners) & set(driver_owners) or required & driver_required:
            raise ValueError("overlapping native CPU and driver inventories")
        owners += driver_owners
        required |= driver_required
    try:
        expanded = {name.format(backend=backend, backend_title=backend.title())
                    for name in required}
    except (KeyError, ValueError, IndexError, AttributeError) as error:
        raise ValueError("invalid native backend placeholder") from error
    if len(expanded) != len(required):
        raise ValueError("overlapping native backend requirements")
    return owners, expanded


def validate_native_host(root: Path, backend: str, system: str, architecture: str) -> None:
    definitions = (root / "scripts" / "NativeCPUTests.def").read_text(encoding="utf-8")
    hosts = re.findall(
        rf'^NEVERD_NATIVE_CPU_HOST\({backend.upper()},\s*"([^"]+)",'
        r'\s*"([^"]+)",\s*"([^"]+)"\)', definitions, re.M,
    )
    if len(hosts) != 1:
        raise ValueError("expected one native CPU host declaration")
    expected_system, *aliases = hosts[0]
    if system != expected_system or architecture.lower() not in {name.lower() for name in aliases}:
        raise ValueError(f"native {backend.upper()} CPU coverage requires {expected_system} x64")


def darwin_inventory(
    root: Path, backend: str, host_architecture: str,
) -> tuple[list[str], set[str]]:
    if backend not in {"hvf", "kvm", "whp"}:
        raise ValueError("Darwin native coverage requires HVF, KVM or WHP")
    filename = "NativeDarwinTests.def"
    prefix = "NEVERD_NATIVE_DARWIN"
    owners, templates = read_inventory(root, filename, prefix, host_architecture)
    cases = re.findall(
        rf"^[ \t]*{prefix}_CASE\(\s*(\w+)\s*\)[ \t]*$",
        (root / "scripts" / filename).read_text(encoding="utf-8"), re.M,
    )
    if not cases or len(set(cases)) != len(cases):
        raise ValueError("Darwin native coverage requires unique workload cases")
    if any(template.count("{case}") != 1 or template.count("{backend}") != 1
           for template in templates):
        raise ValueError("Darwin native names require a case and backend placeholder")
    required = {
        template.format(case=case, backend=backend)
        for template in templates for case in cases
    }
    return owners, required


def hvf_inventory(
    root: Path, host_architecture: str, transport_only: bool = False,
) -> tuple[list[str], set[str]]:
    owners, required = read_inventory(
        root, "NativeHVFTests.def", "NEVERD_NATIVE_HVF", host_architecture,
    )
    if transport_only:
        owner = "NeverDHvfTests"
        required = {name for name in required if name.startswith(
            ("Hvf.", "HvfExecutor.", "HvfConfiguration.", "HvfIntelOwner."))}
        if owner not in owners or not required:
            raise ValueError("HVF transport profile requires its owner and native cases")
        owners = [owner]
    return owners, required


def run(
    build: Path, evidence: Path, parallel: int, require_whp: bool,
    with_drivers: bool = False, require_hvf: bool = False,
    darwin_backend: str | None = None,
    hvf_transport_only: bool = False,
    execution_methods: bool = False,
    hvf_shard: tuple[int, int] | None = None,
    require_kvm: bool = False,
) -> int:
    if require_kvm and (require_whp or require_hvf or darwin_backend):
        raise ValueError("KVM coverage is a separate native CPU profile")
    if hvf_transport_only and not require_hvf:
        raise ValueError("HVF transport profile requires --require-hvf")
    if darwin_backend and (require_whp or require_hvf or with_drivers):
        raise ValueError("Darwin workload coverage is a separate native profile")
    if require_hvf and (require_whp or with_drivers):
        raise ValueError("HVF coverage is a separate native CPU profile")
    if hvf_shard is not None and (not require_hvf or hvf_transport_only or not execution_methods):
        raise ValueError("HVF shards require the complete HVF profile and method execution")
    host_architecture = platform.machine()
    if require_kvm or require_whp:
        validate_native_host(ROOT, "kvm" if require_kvm else "whp",
                             platform.system(), host_architecture)
    if darwin_backend:
        owners, required = darwin_inventory(ROOT, darwin_backend, host_architecture)
    elif require_hvf:
        owners, required = hvf_inventory(ROOT, host_architecture, hvf_transport_only)
    else:
        owners, required = declared_inventory(ROOT, with_drivers, "kvm" if require_kvm else "whp")
    required_hardware = require_kvm or require_whp or require_hvf or bool(darwin_backend)
    native_name = (darwin_backend or ("hvf" if require_hvf else "kvm" if require_kvm else "whp")).upper()
    output_limits = re.findall(
        r"^NEVERD_NATIVE_CPU_OUTPUT_LIMIT\(([1-9][0-9]*)\)$",
        (ROOT / "scripts" / "NativeCPUTests.def").read_text(encoding="utf-8"),
        re.M,
    )
    if len(output_limits) != 1:
        raise ValueError("expected one native test output limit")
    unit_directory = (build / "unittests").resolve()
    configured = set()
    for line in (build / "CMakeFiles" / "TargetDirectories.txt").read_text(
            encoding="utf-8").splitlines():
        target = Path(line.replace("\\", "/"))
        if not target.is_absolute():
            continue
        target = target.resolve()
        if (not target.is_relative_to(unit_directory)
                or target.parent.name != "CMakeFiles" or target.suffix != ".dir"
                or target.stem not in owners):
            continue
        if target.stem in configured:
            raise ValueError(f"ambiguous native CPU owner: {target.stem}")
        configured.add(target.stem)
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
        "ctest", "--test-dir", str(unit_directory),
        "--build-config", "Release", "-L", labels,
    ]
    inventory = subprocess.check_output([*base, "--show-only=json-v1"], text=True)
    (evidence / "inventory.json").write_text(inventory, encoding="utf-8")
    document = json.loads(inventory)
    tests = parse_inventory(document)
    registered_owners = {label for test in tests for label in test.labels}
    missing_owners = set(owners) - registered_owners
    if missing_owners:
        raise ValueError(f"owners have no registered tests: {sorted(missing_owners)}")
    required_missing = required - {test.name for test in tests}
    if required_hardware and required_missing:
        raise ValueError(
            f"missing required native {native_name} tests: {sorted(required_missing)}"
        )
    full_registered = len(tests)
    if hvf_shard is not None:
        # Check the complete registrations before selecting a shard. Deleting a
        # required case must never make it disappear from a shard's obligations.
        document = shard_inventory(document, *hvf_shard)
        (evidence / "full-inventory.json").write_text(inventory, encoding="utf-8")
        (evidence / "inventory.json").write_text(
            json.dumps(document, indent=2) + "\n", encoding="utf-8")
        tests = parse_inventory(document)
        required &= {test.name for test in tests}
    junit = evidence / "results.xml"
    environment = dict(os.environ)
    if require_hvf or darwin_backend == "hvf":
        environment["NEVERD_REQUIRE_HVF"] = "1"
    if require_whp or darwin_backend == "whp":
        environment["NEVERD_REQUIRE_NATIVE_WHP"] = "1"
    if execution_methods:
        cases, execution_status = run_methods(document, evidence, environment)
    else:
        result = subprocess.run([
            *base, "--no-tests=error", "--parallel", str(parallel),
            "--output-on-failure", "--output-junit", str(junit),
            "--test-output-size-passed", output_limits[0],
            "--test-output-size-failed", output_limits[0],
            "--output-log", str(evidence / "ctest.log"),
        ], env=environment)
        cases = parse_junit(ET.parse(junit).getroot())
        execution_status = result.returncode
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
        "host_architecture": host_architecture,
        "host_platform": {
            "system": platform.system(),
            "release": platform.release(),
            "version": platform.version(),
        },
        "logical_cpus": os.cpu_count(),
        "parallel": 1 if execution_methods else parallel,
        "execution": "gtest-methods" if execution_methods else "ctest",
        "owners": owners,
        "registered": len(tests),
        "total": len(cases),
        "counts": {name: counts[name] for name in OUTCOME_NAMES},
        "missing": sorted(test.name for test in expected - actual),
        "unexpected": sorted(test.name for test in actual - expected),
        "require_whp": require_whp,
        "require_kvm": require_kvm,
        "require_hvf": require_hvf,
        "hvf_transport_only": hvf_transport_only,
        "hvf_shard": (None if hvf_shard is None else
                      {"index": hvf_shard[0], "count": hvf_shard[1]}),
        "full_registered": full_registered,
        "darwin_backend": darwin_backend,
        "with_drivers": with_drivers,
        "required_native_tests": len(required),
        "required_native_names": sorted(required),
        "required_native_missing": sorted(required_missing),
        "required_native_unexecuted": required_unexecuted,
        "ctest_status": None if execution_methods else execution_status,
        "execution_status": execution_status,
    }
    (evidence / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary), flush=True)
    return int(bool(
        execution_status or counts["failed"] or counts["disabled"]
        or counts["not_run"] or expected != actual or len(cases) != len(tests)
        or (required_hardware and required_unexecuted)
    ))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--parallel", type=int, default=4)
    parser.add_argument("--require-whp", action="store_true")
    parser.add_argument("--require-kvm", action="store_true")
    parser.add_argument("--require-hvf", action="store_true")
    parser.add_argument("--hvf-transport-only", action="store_true",
                        help="only build and require the small HVF transport owner")
    parser.add_argument("--require-darwin-backend", choices=("hvf", "kvm", "whp"),
                        help="require every native Darwin workload on the host ISA")
    parser.add_argument("--with-drivers", action="store_true")
    parser.add_argument("--execution-methods", action="store_true",
                        help="execute the full CTest inventory in serial, bounded GoogleTest method processes")
    parser.add_argument("--hvf-shard", metavar="INDEX/COUNT",
                        help="execute one zero-based full HVF method shard; all shards require independent aggregation")
    args = parser.parse_args()
    if args.parallel < 1:
        parser.error("parallel jobs must be positive")
    shard = None
    if args.hvf_shard is not None:
        if not re.fullmatch(r"[0-9]+/[0-9]+", args.hvf_shard):
            parser.error("HVF shard must be INDEX/COUNT")
        shard = tuple(map(int, args.hvf_shard.split("/")))
        if shard[1] < 2 or not 0 <= shard[0] < shard[1]:
            parser.error("HVF shard requires 0 <= index < count and count >= 2")
    return run(
        args.build.resolve(), args.evidence.resolve(), args.parallel,
        args.require_whp, args.with_drivers, args.require_hvf,
        args.require_darwin_backend,
        args.hvf_transport_only,
        args.execution_methods,
        shard,
        args.require_kvm,
    )


if __name__ == "__main__":
    sys.exit(main())
