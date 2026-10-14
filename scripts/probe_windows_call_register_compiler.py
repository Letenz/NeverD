#!/usr/bin/env python3
"""Isolate the MSVC 19.44 CallRegisterEffects C1001 on disposable CI source."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def variants(text, newline):
    mapped = "std::map<size_t, llvm::ArrayRef<uint64_t>>"
    marker = mapped + newline + "unreadSyscallArguments"
    begin = text.index(marker)
    end = text.index("/// The scalar writes", begin)
    prefix = text[:text.index("namespace neverd {")]
    rest = ("const llvm::ArrayRef<uint64_t> Rest =\n"
            "        llvm::ArrayRef(kLinuxX64SyscallArgumentRegisters).drop_front(*Count);").replace("\n", newline)
    if text.count(mapped) != 3 or text.count(rest) != 1:
        raise ValueError("Expected the three original map sites and exact register slice")
    span = text.replace("#include <optional>", "#include <optional>" + newline + "#include <span>")
    span = span.replace(mapped, "std::map<size_t, std::span<const uint64_t>>")
    span = span.replace(rest, ("const std::span<const uint64_t> Rest =\n"
        "        std::span<const uint64_t>(kLinuxX64SyscallArgumentRegisters).subspan(*Count);").replace("\n", newline))
    counts = text.replace(mapped, "std::map<size_t, unsigned>").replace(rest, "")
    counts = counts.replace("Unread[I] = Rest;", "Unread[I] = *Count;")
    counts = counts.replace("llvm::any_of(Unread->second,", "llvm::any_of(llvm::ArrayRef(kLinuxX64SyscallArgumentRegisters).drop_front(Unread->second),")
    array_map = prefix + newline + "namespace neverd {" + newline + mapped + " minimalArrayMap() { return {}; }" + newline + "}" + newline
    span_map = array_map.replace("#include <optional>", "#include <optional>" + newline + "#include <span>").replace(mapped, "std::map<size_t, std::span<const uint64_t>>")
    isolated = prefix + newline + "namespace neverd {" + newline + text[begin:end] + newline + "}" + newline
    return [
        ("original-o2", text, ""),
        ("minimal-arrayref-map", array_map, ""),
        ("minimal-span-map", span_map, ""),
        ("isolated-function", isolated, ""),
        ("span-whole-file", span, ""),
        ("count-index-whole-file", counts, ""),
    ]


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
    source = Path("lib/ir/low/CallRegisterEffects.cpp").resolve()
    if str(source).replace("\\", "/").lower() not in command.replace("\\", "/").lower():
        raise ValueError("Compiler command does not consume the isolated source path")
    original = source.read_bytes()
    text = original.decode("utf-8")
    newline = "\r\n" if b"\r\n" in original else "\n"
    results = []
    try:
        for name, body, options in variants(text, newline):
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
                   "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                   "object_written": (build / (name + ".obj")).is_file()}
            results.append(row)
            print(json.dumps(row), flush=True)
    finally:
        source.write_bytes(original)
        (build / "compiler-probe.json").write_text(
            json.dumps(results, indent=2) + "\n", encoding="utf-8")
    if not results or not any(row["returncode"] == 0 and row["object_written"] for row in results):
        raise SystemExit("No successful compilation; inspect the independent variant logs")


if __name__ == "__main__":
    main()
