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
    any_of = ("const bool Syscall =\n"
        "        std::any_of(Block.Ops.begin() + First, Block.Ops.begin() + End,\n"
        "                    [](const LowOp &Op) {\n"
        "                      return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs > 0 &&\n"
        "                             Op.Inputs[0].isConst() &&\n"
        "                             static_cast<Intrinsic>(Op.Inputs[0].Offset) ==\n"
        "                                 Intrinsic::X64Syscall;\n"
        "                    });").replace("\n", newline)
    loop = ("bool Syscall = false;\n"
        "    for (size_t I = First; I < End; ++I) {\n"
        "      const LowOp &Op = Block.Ops[I];\n"
        "      if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs > 0 &&\n"
        "          Op.Inputs[0].isConst() &&\n"
        "          static_cast<Intrinsic>(Op.Inputs[0].Offset) == Intrinsic::X64Syscall) {\n"
        "        Syscall = true;\n"
        "        break;\n"
        "      }\n"
        "    }").replace("\n", newline)
    result_type = "std::map<size_t, llvm::ArrayRef<uint64_t>>" + newline + "unreadSyscallArguments"
    alias_type = ("using SyscallRegisterSlices = std::map<size_t, llvm::ArrayRef<uint64_t>>;\n"
                  "SyscallRegisterSlices\nunreadSyscallArguments").replace("\n", newline)
    if text.count(any_of) != 1 or text.count(result_type) != 1:
        raise ValueError("Expected the original independent loop and result-type sites")
    begin = text.index(result_type)
    end = text.index("/// The scalar writes", begin)
    moved = text[:begin] + text[begin:end].replace("return Unread;", "return std::move(Unread);") + text[end:]
    variants = [
        ("original-o2", text, ""),
        ("original-no-nrvo", text, " /Zc:nrvo-"),
        ("explicit-loop-o2", text.replace(any_of, loop), ""),
        ("named-result-type-o2", text.replace(result_type, alias_type), ""),
        ("runtime-arch-count-o2", text.replace("linuxX64SyscallArgumentCount(*Number)", "linuxSyscallArgumentCount(Img.Arch, *Number)"), ""),
        ("explicit-move-o2", moved, ""),
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
    if not results or not any(row["returncode"] == 0 and row["object_written"] for row in results):
        raise SystemExit("No successful compilation; inspect the independent variant logs")


if __name__ == "__main__":
    main()
