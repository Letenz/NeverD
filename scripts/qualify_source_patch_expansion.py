#!/usr/bin/env python3
"""Qualify one native host against the checked-in expansion test inventory."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import xml.etree.ElementTree as ET

from run_lift_qualification import audit
from check_source_patch_expansion_runtime import native_architecture

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--clang", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--expected-arch", required=True, choices=("x64", "arm64"))
    args = parser.parse_args()
    actual = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
    ).strip()
    dirty = subprocess.check_output(
        ["git", "status", "--porcelain", "--untracked-files=no"], cwd=ROOT, text=True
    )
    if actual != args.source_commit or dirty:
        raise ValueError("qualification requires the exact clean source commit")
    system = platform.system()
    architecture = native_architecture(platform.machine())
    if architecture != args.expected_arch:
        raise ValueError("unexpected native architecture")
    inventory = json.loads(
        (ROOT / "unittests/Inputs/source-patch-expansion.json").read_text()
    )
    cases = dict(inventory["common"])
    cases["NeverDSourcePatchTests"] = inventory["native"][system + "/" + architecture]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {
        "source_commit": actual,
        "source_dirty": False,
        "system": system,
        "architecture": architecture,
        "harness_commit": os.environ.get("GITHUB_SHA"),
        "run_id": os.environ.get("GITHUB_RUN_ID"),
        "run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
        "checks": {},
        "binaries": {},
    }
    for owner, names in cases.items():
        binary = (
            args.build.resolve()
            / "bin"
            / (owner + (".exe" if system == "Windows" else ""))
        )
        xml = output / (owner + ".xml")
        with (output / (owner + ".log")).open("w") as log:
            result = subprocess.run(
                [
                    str(binary),
                    "--gtest_filter=" + ":".join(names),
                    "--gtest_output=xml:" + str(xml),
                ],
                stdout=log,
                stderr=subprocess.STDOUT,
                env=dict(os.environ, NEVERD_TEST_REQUIRE_FIXED_LLVM="1"),
                timeout=1200,
            )
        report["checks"][owner] = audit(
            ET.parse(xml).getroot(), names, result.returncode
        )
        report["binaries"][owner] = hashlib.sha256(binary.read_bytes()).hexdigest()
        print(f"{owner}: {len(names)} passed, zero failed or skipped", flush=True)
    library = (
        "neverd.dll"
        if system == "Windows"
        else "libneverd.dylib"
        if system == "Darwin"
        else "libneverd.so"
    )
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts/check_source_patch_expansion_runtime.py"),
            "--library",
            str(args.build.resolve() / "bin" / library),
            "--clang",
            str(args.clang.resolve()),
            "--output",
            str(output / "native"),
            "--expected-arch",
            architecture,
            "--source-commit",
            actual,
        ],
        check=True,
    )
    report["files"] = {
        p.relative_to(output).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(output.rglob("*"))
        if p.is_file()
    }
    (output / "qualification.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
