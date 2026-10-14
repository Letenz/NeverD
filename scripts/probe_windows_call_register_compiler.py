#!/usr/bin/env python3
"""Isolate the MSVC 19.44 CallRegisterEffects C1001 on disposable CI source."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def baseline_source(text, newline):
    mapped = "std::map<size_t, llvm::ArrayRef<uint64_t>>"
    span_mapped = "std::map<size_t, std::span<const uint64_t>>"
    if text.count(mapped) == 0 and text.count(span_mapped) == 3:
        text = text.replace(span_mapped, mapped).replace("#include <span>" + newline, "")
        rest = ("const llvm::ArrayRef<uint64_t> Rest =\n"
                "        llvm::ArrayRef(kLinuxX64SyscallArgumentRegisters).drop_front(*Count);").replace("\n", newline)
        text, count = re.subn(
            r"const std::span<const uint64_t> Rest =\s*std::span<const uint64_t>\(kLinuxX64SyscallArgumentRegisters\)\s*\.subspan\(\*Count\);",
            lambda _: rest, text)
        if count != 1:
            raise ValueError("Expected the exact standard-span register slice")
    return text


def variants(text, newline):
    text = baseline_source(text, newline)
    mapped = "std::map<size_t, llvm::ArrayRef<uint64_t>>"
    marker = mapped + newline + "unreadSyscallArguments"
    begin = text.index(marker)
    end = text.index("/// The scalar writes", begin)
    prefix = text[:text.index("namespace neverd {")]
    close = newline + "} // namespace" + newline + "} // namespace neverd" + newline
    save_begin = text.index("std::set<std::pair<size_t, size_t>>")
    save_end = text.index("/// The byte and word writes", save_begin)
    masked_begin = text.index("void findMaskedMerges")
    masked_end = text.index("/// The argument registers", masked_begin)

    def stub(start, stop, result):
        signature_end = text.index("{", start, stop)
        return text[start:signature_end] + "{ " + result + " }" + newline + newline

    no_save = text[:save_begin] + stub(save_begin, save_end, "return {};") + text[save_end:]
    no_masked = text[:masked_begin] + stub(masked_begin, masked_end, "") + text[masked_end:]
    no_both = (text[:save_begin] + stub(save_begin, save_end, "return {};") +
               text[save_end:masked_begin] + stub(masked_begin, masked_end, "") + text[masked_end:])
    no_unread = text[:begin] + stub(begin, end, "return {};") + text[end:]
    first_helper = text.index("GPRFamilyMask familyBit")
    moved = text[:first_helper] + text[begin:end] + text[first_helper:begin] + text[end:]
    isolated = prefix + newline + "namespace neverd { namespace {" + newline + text[begin:end] + close
    return [
        ("original-o2", text, ""),
        ("through-masked-merges", text[:masked_end] + close, ""),
        ("through-unread", text[:end] + close, ""),
        ("isolated-in-anonymous", isolated, ""),
        ("without-save-area-body", no_save, ""),
        ("without-masked-merge-body", no_masked, ""),
        ("without-both-earlier-bodies", no_both, ""),
        ("without-unread-body", no_unread, ""),
        ("unread-before-earlier-functions", moved, ""),
        ("original-syntax-only", text, " /Zs"),
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
