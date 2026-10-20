#!/usr/bin/env python3
"""Statically verify compiler-generated PE32 cleanup-only LLVM lowering."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

if __package__:
    from .check_windows_registration_realigned import require_test_result
    from .windows_registration_libraries import load_libraries
else:
    from check_windows_registration_realigned import require_test_result
    from windows_registration_libraries import load_libraries

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_cleanup_only.cpp"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", type=Path, required=True)
    parser.add_argument("--runtime-libs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--linker", default="lld-link")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"schema": 1, "evidence": "static-cleanup-only-source-to-llvm",
              "passed": False, "commands": [], "cases": [],
              "source_sha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest()}

    def run(command: list[str | Path], extra: dict | None = None) -> None:
        command = list(map(str, command))
        result = subprocess.run(command, capture_output=True, text=True,
                                errors="replace", timeout=120, cwd=output,
                                env=os.environ | (extra or {}))
        report["commands"].append({"command": command,
                                   "exit_code": result.returncode,
                                   "stdout": result.stdout,
                                   "stderr": result.stderr})
        if result.returncode:
            raise ValueError("cleanup-only fixture build or proof failed")

    try:
        libraries, report["runtime_libraries"] = load_libraries(
            args.runtime_libs.resolve())
        for optimization in ("O0", "O1"):
            case = output / optimization
            case.mkdir(exist_ok=True)
            image, obj, xml = (case / name for name in
                               ("original.exe", "source.obj", "proof.xml"))
            record = {"optimization": optimization, "passed": False}
            report["cases"].append(record)
            run([args.compiler, "--target=i686-pc-windows-msvc",
                 "-fms-extensions", "-fexceptions", "-fcxx-exceptions",
                 "-fno-omit-frame-pointer", "-" + optimization,
                 "-c", SOURCE, "-o", obj])
            run([args.linker, "/entry:mainCRTStartup", "/nodefaultlib",
                 "/machine:x86", "/subsystem:console", "/fixed:no",
                 "/dynamicbase:no", "/out:" + str(image), obj, *libraries])
            record["image_sha256"] = hashlib.sha256(image.read_bytes()).hexdigest()
            run([args.test_binary.resolve(),
                 "--gtest_filter=WindowsRegistrationCleanupOnly.*",
                 "--gtest_output=xml:" + str(xml)],
                {"NEVERD_REGISTRATION_CLEANUP_ONLY_PE32": str(image)})
            if require_test_result(xml) != 1:
                raise ValueError("cleanup-only source proof missing or skipped")
            record["passed"] = True
        report["passed"] = True
    except (OSError, ValueError, subprocess.TimeoutExpired, ET.ParseError) as error:
        report["error"] = str(error)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("cleanup-only static proof:", "PASS" if report["passed"] else "FAIL")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
