#!/usr/bin/env python3
"""Test an x64 LLVM/CRT resume protocol; this grants no reconstruction authority."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess

if __package__:
    from .check_windows_nonlocal_finally import require_architecture
    from .check_windows_registration_eh import run_image
else:
    from check_windows_nonlocal_finally import require_architecture
    from check_windows_registration_eh import run_image

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "unittests/lift/eh/fixtures"
SOURCES = ("native_seh_resume.ll", "native_seh_resume_runtime.c",
           "native_seh_resume_probe.asm")
EXPECTED = (123478, 12378, 13678, 1378, 1, 1, 1, 1, 128)
OBSERVATION = re.compile(
    r"NATIVE_SEH_RESUME (\d+) (\d+) (\d+) (\d+) registers=(\d+) "
    r"stack=(\d+) guards=(\d+) consistent=(\d+) iterations=(\d+) "
    r"runtime=([^\r\n]+)\s*")


def observe(result: dict) -> tuple[int, ...]:
    match = OBSERVATION.fullmatch(result.get("stdout", ""))
    if result.get("exit_code") != 0 or not match:
        raise ValueError("native resume protocol execution did not complete")
    values = tuple(map(int, match.groups()[:9]))
    if values != EXPECTED:
        raise ValueError("native continuation, second exception, ABI or guard differs")
    return values


def compile_step(command: list[str], directory: Path, record: dict) -> None:
    result = subprocess.run(command, cwd=directory, capture_output=True,
                            text=True, errors="replace", timeout=120)
    record.setdefault("compiler", []).append(
        {"command": command, "exit_code": result.returncode,
         "stdout": result.stdout, "stderr": result.stderr})
    if result.returncode:
        raise ValueError("native resume protocol compilation failed")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--compiler", default="cl")
    parser.add_argument("--llvm-compiler", default="clang")
    parser.add_argument("--assembler", default="ml64")
    args = parser.parse_args(argv)
    if os.name != "nt":
        parser.error("this protocol experiment requires native Windows x64")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"schema": 1, "evidence": "experimental-native-x64-crt-resume",
              "platform": platform.platform(), "architecture": "x64",
              "toolset": os.environ.get("VCToolsVersion", ""),
              "source_sha256": {name: hashlib.sha256(
                  (FIXTURES / name).read_bytes()).hexdigest() for name in SOURCES},
              "passed": False, "reconstruction_authority": False, "cases": []}
    try:
        if os.environ.get("VSCMD_ARG_TGT_ARCH") != "x64" or not report["toolset"]:
            raise ValueError("select the native x64 MSVC environment first")
        version = subprocess.run([args.llvm_compiler, "--version"],
                                 capture_output=True, text=True, errors="replace",
                                 timeout=30)
        report["llvm_toolchain"] = {"command": [args.llvm_compiler, "--version"],
                                    "exit_code": version.returncode,
                                    "stdout": version.stdout, "stderr": version.stderr}
        if version.returncode:
            raise ValueError("LLVM protocol compiler is unavailable")
        for optimization in (0, 2):
            case = output / f"O{optimization}"
            case.mkdir(exist_ok=True)
            record = {"case": case.name, "passed": False}
            report["cases"].append(record)
            llvm_object, wrapper = case / "resume.obj", case / "probe.obj"
            image = case / "resume.exe"
            compile_step([args.llvm_compiler, "-target", "x86_64-pc-windows-msvc",
                          f"-O{optimization}", "-Werror", "-x", "ir", "-c",
                          str(FIXTURES / SOURCES[0]), "-o", str(llvm_object)],
                         case, record)
            compile_step([args.assembler, "/nologo", "/c", f"/Fo{wrapper}",
                          str(FIXTURES / SOURCES[2])], case, record)
            compile_step([args.compiler, "/nologo", "/W4", "/WX", "/TC",
                          "/Od" if optimization == 0 else "/O2", "/GS", "/MD", "/Z7",
                          f"/Fo{case / 'runtime.obj'}", f"/Fe{image}",
                          str(FIXTURES / SOURCES[1]), str(llvm_object), str(wrapper),
                          "/link", "/debug", "/incremental:no", "/fixed:no",
                          f"/pdb:{case / 'resume.pdb'}", f"/map:{case / 'resume.map'}"],
                         case, record)
            data = image.read_bytes()
            require_architecture(data, "x64")
            record["sha256"] = hashlib.sha256(data).hexdigest()
            record["runtime"] = run_image(image, [], os.environ.copy(), 60)
            record["values"] = observe(record["runtime"])
            match = OBSERVATION.fullmatch(record["runtime"]["stdout"])
            runtime_path = Path(match.group(10).strip())
            if not runtime_path.is_absolute() or runtime_path.name.lower() != "vcruntime140.dll":
                raise ValueError("native resume has no identified VCRUNTIME140 module")
            runtime_data = runtime_path.read_bytes()
            require_architecture(runtime_data, "x64")
            runtime_digest = hashlib.sha256(runtime_data).hexdigest()
            provider_copy = output / "runtime-providers" / runtime_digest / "vcruntime140.dll"
            provider_copy.parent.mkdir(parents=True, exist_ok=True)
            provider_copy.write_bytes(runtime_data)
            record["loaded_provider"] = {"name": runtime_path.name,
                                          "path": str(runtime_path),
                                          "sha256": runtime_digest,
                                          "captured": str(provider_copy)}
            record["passed"] = True
        report["passed"] = True
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    destination = output / "native-seh-resume.json"
    destination.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "report": str(destination),
                      "error": report.get("error"), "reconstruction_authority": False}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
