#!/usr/bin/env python3
"""Isolate the MSVC 19.44 CallRegisterEffects C1001 on disposable CI source."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    args = parser.parse_args()
    if os.name != "nt":
        raise SystemExit("This probe requires the native Windows MSVC environment")
    build = args.build.resolve()
    target = "lib/ir/low/CMakeFiles/NeverDIRLow.dir/CallRegisterEffects.cpp.obj"
    commands = subprocess.check_output(
        ["ninja", "-C", str(build), "-t", "commands", target], text=True)
    command = commands.strip().splitlines()[-1]
    if "cl.exe" not in command or "CallRegisterEffects.cpp" not in command:
        raise ValueError("Expected the configured production MSVC command")
    source = Path("lib/ir/low/CallRegisterEffects.cpp")
    original = source.read_bytes()
    text = original.decode("utf-8")
    newline = "\r\n" if b"\r\n" in original else "\n"
    ctad = "llvm::ArrayRef(kLinuxX64SyscallArgumentRegisters)"
    count = ("const std::optional<unsigned> Count =\n"
             "        Number ? linuxX64SyscallArgumentCount(*Number) : std::nullopt;").replace("\n", newline)
    if text.count(ctad) != 1 or text.count(count) != 1:
        raise ValueError("The isolated source no longer matches the reported failure")
    variants = [
        ("original-o2", text, ""),
        ("original-od", text, " /Od"),
        ("explicit-arrayref-o2", text.replace(ctad, "llvm::ArrayRef<uint64_t>(kLinuxX64SyscallArgumentRegisters)"), ""),
        ("explicit-optional-o2", text.replace(count, (
            "std::optional<unsigned> Count;\n"
            "    if (Number)\n"
            "      Count = linuxX64SyscallArgumentCount(*Number);").replace("\n", newline)), ""),
    ]
    results = []
    try:
        for name, body, options in variants:
            source.write_bytes(body.encode("utf-8"))
            compile_command, replacements = re.subn(
                r'/Fo(?:"[^"]*"|\S+)', "/Fo" + name + ".obj", command, count=1)
            if replacements != 1:
                raise ValueError("Expected one MSVC output object")
            process = subprocess.run(compile_command + options, cwd=build,
                                     shell=True, capture_output=True)
            output = process.stdout + process.stderr
            (build / (name + ".log")).write_bytes(output)
            row = {"variant": name, "returncode": process.returncode,
                   "c1001": b"C1001" in output,
                   "object_written": (build / (name + ".obj")).is_file()}
            results.append(row)
            print(json.dumps(row), flush=True)
    finally:
        source.write_bytes(original)
        (build / "compiler-probe.json").write_text(
            json.dumps(results, indent=2) + "\n", encoding="utf-8")
    if not results or not any(row["returncode"] == 0 for row in results):
        raise SystemExit("No successful compilation; inspect the independent variant logs")


if __name__ == "__main__":
    main()
