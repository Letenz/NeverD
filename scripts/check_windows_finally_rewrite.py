#!/usr/bin/env python3
"""Reconstruct a genuine MSVC PE32 nonlocal finally and compare its execution.

The checked export is loaded under an outer SEH observer. Header-only
EXE-to-DLL adaptation leaves source code intact and avoids unrelated CRT startup.
The observer checks return values, the exception chain and stack guards.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import xml.etree.ElementTree as ET

if __package__:
    from .check_windows_registration_eh import run_image
    from .windows_registration_runtime import load_runtime, validate_runtime
    from .windows_finally_image import PE32, captured_input, as_library, generated_entry, observe
else:
    from check_windows_registration_eh import run_image
    from windows_registration_runtime import load_runtime, validate_runtime
    from windows_finally_image import PE32, captured_input, as_library, generated_entry, observe

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "unittests/lift/eh/fixtures"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inputs", type=Path, required=True)
    parser.add_argument("--runtime-libs", type=Path, required=True)
    parser.add_argument("--test-binary", type=Path, required=True)
    parser.add_argument("--patch-binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profile", choices=("Od-no-gs", "Od-gs", "O2-no-gs", "O2-gs"), default="Od-no-gs")
    parser.add_argument("--form", choices=("return", "goto"), default="return")
    parser.add_argument("--compiler", default="clang")
    parser.add_argument("--linker", default="lld-link")
    parser.add_argument("--wine")
    parser.add_argument("--wine-prefix", type=Path)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"schema": 1, "evidence": "reconstructed-msvc-finally-" + args.form,
              "profile": "x86/" + args.profile, "passed": False, "steps": [], "observations": []}
    environment = os.environ.copy()

    def run(command):
        result = subprocess.run(command, env=environment, capture_output=True,
                                text=True, errors="replace", timeout=args.timeout)
        report["steps"].append({"command": command, "exit_code": result.returncode,
                                "stdout": result.stdout, "stderr": result.stderr})
        if result.returncode:
            raise ValueError(f"finally reconstruction command failed: {command[0]}")

    try:
        compiler, linker = shutil.which(args.compiler), shutil.which(args.linker)
        if not compiler or not linker:
            raise ValueError("cross-target clang and lld-link are required")
        inputs, libraries = args.inputs.resolve(), args.runtime_libs.resolve()
        source = FIXTURES / "nonlocal_finally_runtime.c"
        original = captured_input(inputs, source, args.profile)
        control = captured_input(inputs, source, "O2-no-gs-control")
        runtime, report["runtime"] = load_runtime(libraries)
        environment["WINEDLLOVERRIDES"] = "vcruntime140=n"
        launcher = []
        if os.name != "nt":
            wine = args.wine or shutil.which("wine")
            if not wine:
                raise ValueError("Wine is unavailable; native finally was not executed")
            launcher = [wine]
            prefix = (args.wine_prefix or output / "wine-prefix").resolve()
            prefix.mkdir(parents=True, exist_ok=True)
            environment.update(WINEPREFIX=str(prefix), WINEARCH="win32", WINEDEBUG="-all")

        environment.update(NEVERD_NONLOCAL_FINALLY_INPUTS=str(inputs),
                           NEVERD_NONLOCAL_FINALLY_OUTPUT=str(output))
        ir_prefix = {"Od-no-gs": "return-", "Od-gs": "return-gs-"}.get(
            args.profile, "return-" + args.profile + "-")
        if args.form == "goto":
            ir_prefix = "goto-" + args.profile + "-"
        export = b"nonlocal_finally_" + args.form.encode("ascii")
        ir_names = [ir_prefix + form + ".ll" for form in ("raw", "opt")]
        for name in ["native-inputs.xml", *ir_names]:
            (output / name).unlink(missing_ok=True)
        run([str(args.test_binary.resolve()),
             "--gtest_filter=PipelineOutcome.NativeFinallyInputsRetainSourceLLVM",
             f"--gtest_output=xml:{output / 'native-inputs.xml'}"])
        tests = ET.parse(output / "native-inputs.xml").getroot()
        if int(tests.get("tests", "0")) != 1 or tests.findall(".//skipped") or \
                tests.findall(".//failure"):
            raise ValueError("native finally source and COFF proofs were not executed")
        for name in ir_names:
            if not (output / name).is_file():
                raise ValueError("native finally proof did not emit both IR variants")

        observer = FIXTURES / "nonlocal_finally_observer.c"
        if args.form == "goto":
            observer = FIXTURES / "nonlocal_finally_goto_observer.c"
        run([compiler, "--target=i686-pc-windows-msvc", "-O1", "-fms-extensions",
             "-fexceptions", "-fno-stack-protector", "-c", str(observer),
             "-o", str(output / "observer.obj")])
        run([linker, "/machine:x86", "/subsystem:console", "/entry:probe_entry",
             "/nodefaultlib", "/safeseh", "/base:0x30000000", "/dynamicbase:no",
             str(output / "observer.obj"), str(libraries / "kernel32.lib"),
             str(libraries / "vcruntime.lib"), f"/out:{output / 'observer.exe'}"])
        report["observer_source_sha256"] = hashlib.sha256(observer.read_bytes()).hexdigest()
        report["observer_sha256"] = hashlib.sha256((output / "observer.exe").read_bytes()).hexdigest()
        images = [("original", original, False, False), ("control", control, False, True)]
        source_data = original.read_bytes()
        for mode in ("section", "inplace"):
            for form in ("raw", "opt"):
                name = mode + "-" + form
                image = output / (name + ".exe")
                options = [f"--from-ir={output / (ir_prefix + form + '.ll')}"]
                if form == "raw":
                    options.append("--no-opt")
                image.unlink(missing_ok=True)
                run([str(args.patch_binary.resolve()), "patch", str(original), *options,
                     f"--mode={mode}", "-o", str(image)])
                generated_entry(source_data, image.read_bytes(), export)
                images.append((name, image, True, False))
        for name, image, generated, negative in images:
            data = image.read_bytes()
            for base in (0x14000000, 0x18000000):
                case = output / f"{name}-{base:x}"
                case.mkdir(exist_ok=True)
                adapted = as_library(PE32(data).rebase(base), export)
                (case / "target.dll").write_bytes(adapted)
                shutil.copy(output / "observer.exe", case / "observer.exe")
                shutil.copy(runtime, case / runtime.name)
                validate_runtime(case, report["runtime"])
                result = run_image(case / "observer.exe", launcher, environment, args.timeout)
                observation = {"case": name, "generated": generated,
                    "negative_control": negative, "base": base,
                    "source_sha256": hashlib.sha256(data).hexdigest(),
                    "loaded_sha256": hashlib.sha256(adapted).hexdigest(),
                    "runtime": result}
                report["observations"].append(observation)
                observation["values"] = observe(result, base, negative, args.form)
        report["passed"] = True
    except (OSError, ValueError, struct.error, ET.ParseError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    destination = output / "finally-rewrite.json"
    destination.write_text(json.dumps(report, indent=2) + "\n")
    print(f"{'PASS' if report['passed'] else 'FAIL'} native MSVC finally: {destination}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
