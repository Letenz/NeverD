#!/usr/bin/env python3
"""Capture original MSVC nonlocal-finally behavior; no reconstruction is implied."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import struct
import subprocess

if __package__:
    from .check_windows_registration_eh import run_image
else:
    from check_windows_registration_eh import run_image

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "unittests/lift/eh/fixtures/nonlocal_finally_runtime.c"
OBSERVATION = re.compile(
    r"NONLOCAL_FINALLY (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) "
    r"guards=(\d+) chain=(\d+) consistent=(\d+) iterations=(\d+)\s*")
EXPECTED = (123457, 12357, 13567, 1357, 123, 13)
CONTROL = (128457, 12857, 18567, 1857, 128, 18)


def observe(result: dict, control: bool = False) -> tuple[int, ...]:
    match = OBSERVATION.fullmatch(result.get("stdout", ""))
    if result.get("exit_code") != 0 or not match:
        raise ValueError("nonlocal finally execution did not complete")
    values = tuple(map(int, match.groups()))
    if values != (CONTROL if control else EXPECTED) + (1, 1, 1, 192):
        raise ValueError("finally order, resumption, guards or chain differs")
    return values


def require_architecture(data: bytes, arch: str) -> None:
    machine, magic = {"x86": (0x14C, 0x10B), "x64": (0x8664, 0x20B)}[arch]
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("compiler did not produce a PE image")
    offset = struct.unpack_from("<I", data, 60)[0]
    if offset > len(data) - 26 or data[offset:offset + 4] != b"PE\0\0" or \
            struct.unpack_from("<H", data, offset + 4)[0] != machine or \
            struct.unpack_from("<H", data, offset + 24)[0] != magic:
        raise ValueError("MSVC output has the wrong architecture")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--arch", required=True, choices=("x86", "x64"))
    parser.add_argument("--compiler", default="cl")
    args = parser.parse_args(argv)
    if os.name != "nt":
        parser.error("this baseline requires native Windows MSVC")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"schema": 1, "evidence": "original-msvc-nonlocal-finally",
              "platform": platform.platform(), "architecture": args.arch,
              "toolset": os.environ.get("VCToolsVersion", ""),
              "source_sha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
              "passed": False, "cases": []}
    try:
        if os.environ.get("VSCMD_ARG_TGT_ARCH") != args.arch or not report["toolset"]:
            raise ValueError("select the requested native MSVC environment first")
        profiles = [(opt, gs, False) for opt in ("Od", "O2") for gs in (False, True)]
        profiles.append(("O2", False, True))
        for opt, gs, control in profiles:
            name = f"{opt}-{'gs' if gs else 'no-gs'}" + ("-control" if control else "")
            case = output / name
            case.mkdir(exist_ok=True)
            image = case / "original.exe"
            record = {"case": name, "passed": False, "negative_control": control}
            report["cases"].append(record)
            # C4532 warns about deliberately leaving a finally during unwind.
            # That documented behavior is the subject of this runtime oracle.
            command = [args.compiler, "/nologo", "/W4", "/WX", "/wd4532", "/TC",
                       "/" + opt, "/GS" if gs else "/GS-", "/Z7", "/MD",
                       f"/DFINALLY_NEGATIVE_CONTROL={int(control)}",
                       f"/Fo{case / 'original.obj'}", f"/Fe{image}", str(SOURCE),
                       "/link", "/debug", "/incremental:no", "/fixed:no",
                       f"/pdb:{case / 'original.pdb'}", f"/map:{case / 'original.map'}"]
            if args.arch == "x86":
                command.insert(command.index("/link"), "/Oy-")
            compiled = subprocess.run(command, cwd=case, capture_output=True,
                                      text=True, errors="replace", timeout=120)
            record["compiler"] = {"command": command, "exit_code": compiled.returncode,
                                  "stdout": compiled.stdout, "stderr": compiled.stderr}
            if compiled.returncode:
                raise ValueError(f"MSVC compilation failed for {name}")
            data = image.read_bytes()
            require_architecture(data, args.arch)
            record["sha256"] = hashlib.sha256(data).hexdigest()
            record["runtime"] = run_image(image, [], os.environ.copy(), 60)
            record["values"] = observe(record["runtime"], control)
            record["passed"] = True
        report["passed"] = True
    except (OSError, ValueError, struct.error, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    destination = output / "nonlocal-finally.json"
    destination.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "report": str(destination),
                      "error": report.get("error")}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
