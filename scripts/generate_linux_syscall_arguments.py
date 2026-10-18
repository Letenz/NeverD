#!/usr/bin/env python3
"""Generate include/neverd/ir/intrinsics/LinuxSyscallArguments.def.

A Linux system call reads its number and then as many argument registers as
the kernel's handler takes: `getpid` none, `read` three.  The lift models a
call whose number it knows as reading only those, so a wrapper does not take
the other argument registers as parameters.  This script counts them, for
x86-64, AArch64 and 32-bit ARM (EABI), from a kernel source tree: the
architecture's system call table names each number's handler, and the
handler's prototype in <linux/syscalls.h>, or the architecture's own
definition of it, gives its parameters.  On ARM a 64-bit parameter takes an
even-aligned register pair, so `pread64` reads r0-r5.  Only the numbers and
the counts are read from the kernel; the system call interface is its ABI.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = (ROOT / "include" / "neverd" / "ir" / "intrinsics" /
          "LinuxSyscallArguments.def")

# Parameter types an ARM EABI call passes in an even-aligned register pair.
WIDE_TYPES = {"loff_t", "u64", "__u64", "s64", "__s64", "long long",
              "unsigned long long", "compat_arg_u64"}

# ARM handlers the system call table names by an assembly entry point
# (arch/arm/kernel/entry-common.S) that takes the C handler's arguments.
ARM_ENTRY_ALIASES = {
    "sys_sigreturn_wrapper": "sys_sigreturn",
    "sys_rt_sigreturn_wrapper": "sys_rt_sigreturn",
    "sys_statfs64_wrapper": "sys_statfs64",
    "sys_fstatfs64_wrapper": "sys_fstatfs64",
    "sys_mmap2": "sys_mmap_pgoff",
}


def split_parameters(text):
    """The parameter declarations of a prototype's parameter list."""
    text = " ".join(text.split())
    if text in ("", "void"):
        return []
    parameters, depth, current = [], 0, ""
    for char in text:
        if char == "," and depth == 0:
            parameters.append(current.strip())
            current = ""
            continue
        depth += char == "("
        depth -= char == ")"
        current += char
    parameters.append(current.strip())
    return parameters


def parameter_type(declaration):
    """The type of one parameter declaration, without its name."""
    words = declaration.replace("*", " * ").split()
    words = [w for w in words if w not in ("const", "__user", "volatile")]
    if len(words) > 1 and words[-1] != "*" and not words[-1].endswith("_t") \
            and words[-1] not in ("int", "long", "char", "short", "u32",
                                  "u64", "s64", "__u64", "loff_t"):
        words = words[:-1]
    return " ".join(words)


def prototypes(tree):
    """Each handler's parameter types, from <linux/syscalls.h>, the
    architectures' C definitions and SYSCALL_DEFINEn."""
    found = {}
    asmlinkage = re.compile(r"asmlinkage\s+long\s+(sys_\w+)\s*\((.*?)\)\s*[;{]",
                            re.S)
    text = (tree / "include/linux/syscalls.h").read_text(errors="replace")
    for match in asmlinkage.finditer(text):
        found.setdefault(match.group(1), [
            parameter_type(p) for p in split_parameters(match.group(2))])
    define = re.compile(r"SYSCALL_DEFINE(\d)\((\w+)((?:[^()]|\([^()]*\))*)\)")
    for directory in ("arch/x86/kernel", "arch/arm/kernel", "arch/arm64/kernel",
                      "kernel", "mm", "fs", "ipc", "net", "security"):
        for path in sorted((tree / directory).rglob("*.c")):
            source = path.read_text(errors="replace")
            for match in asmlinkage.finditer(source):
                found.setdefault(match.group(1), [
                    parameter_type(p)
                    for p in split_parameters(match.group(2))])
            for match in define.finditer(source):
                count, name = int(match.group(1)), match.group(2)
                fields = split_parameters(match.group(3).lstrip(","))
                types = [" ".join(fields[i].split()) for i in range(0, 2 * count, 2)
                         if i < len(fields)]
                if len(types) == count:
                    found.setdefault("sys_" + name, types)
    return found


def registers(types, pairs):
    """The argument registers a handler of \\p types reads."""
    count = 0
    for type_ in types:
        if pairs and type_ in WIDE_TYPES:
            count += count % 2 + 2
        else:
            count += 1
    return count


def table(path, abis):
    """The (number, name, handler) rows of a system call table."""
    rows = []
    for line in path.read_text().splitlines():
        line = line.split("#")[0].strip()
        fields = line.split()
        if len(fields) >= 4 and fields[1] in abis:
            rows.append((int(fields[0]), fields[2], fields[3]))
    return rows


def generic_table(tree):
    """AArch64's (number, name, handler) rows: scripts/syscall.tbl's, or those
    asm-generic/unistd.h declares for a 64-bit kernel."""
    script = tree / "scripts/syscall.tbl"
    if script.exists():
        return table(script, {"common", "64", "renameat", "rlimit",
                              "memfd_secret"})
    text = (tree / "include/uapi/asm-generic/unistd.h").read_text()
    numbers = {name: int(value) for name, value in re.findall(
        r"#define\s+(__NR(?:3264)?_\w+)\s+(\d+)\b", text)}
    rows = []
    entry = re.compile(r"__(SYSCALL|SC_COMP|SC_3264|SC_COMP_3264)\((__NR\w+),"
                       r"\s*(\w+)(?:,\s*(\w+))?(?:,\s*(\w+))?\)")
    for kind, macro, first, second, _ in entry.findall(text):
        if macro not in numbers:
            continue
        handler = second if kind in ("SC_3264", "SC_COMP_3264") else first
        name = macro.replace("__NR3264_", "").replace("__NR_", "")
        rows.append((numbers[macro], name, handler))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--kernel", type=Path, required=True,
                        help="a Linux source tree")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()
    tree = args.kernel
    version = "unknown"
    makefile = (tree / "Makefile").read_text(errors="replace")
    parts = dict(re.findall(r"^(VERSION|PATCHLEVEL)\s*=\s*(\d+)", makefile,
                            re.M))
    if parts:
        version = f"{parts.get('VERSION')}.{parts.get('PATCHLEVEL')}"
    known = prototypes(tree)
    arches = [
        ("X64", table(tree / "arch/x86/entry/syscalls/syscall_64.tbl",
                      {"common", "64"}), False, {}),
        ("AArch64", generic_table(tree), False, {}),
        ("ARM", table(tree / "arch/arm/tools/syscall.tbl",
                      {"common", "eabi"}), True, ARM_ENTRY_ALIASES),
    ]
    lines = [
        "// The argument registers each Linux system call reads, by architecture",
        "// and number: the count of the kernel handler's parameters, where on",
        "// ARM a 64-bit one takes an even-aligned register pair.  Generated by",
        "// scripts/generate_linux_syscall_arguments.py from Linux "
        f"{version}'s",
        "// system call tables and handler prototypes; regenerate it rather",
        "// than edit it.  A number it does not list keeps every argument",
        "// register.",
        "",
        "#ifndef NEVERD_LINUX_SYSCALL_ARGUMENTS",
        "#define NEVERD_LINUX_SYSCALL_ARGUMENTS(Arch, Number, Count)",
        "#endif",
        "",
    ]
    entries = []
    for arch, rows, pairs, aliases in arches:
        missing = []
        seen = set()
        for number, name, handler in sorted(rows):
            if number in seen:
                continue
            types = known.get(aliases.get(handler, handler))
            if types is None:
                missing.append(handler)
                continue
            seen.add(number)
            entries.append((f"NEVERD_LINUX_SYSCALL_ARGUMENTS({arch}, {number}, "
                            f"{registers(types, pairs)})", name))
        print(f"{arch}: {len(seen)} calls; no prototype for {missing}",
              file=sys.stderr)
    # The names align in a column, as clang-format aligns trailing comments.
    width = max(len(code) for code, _ in entries)
    lines += [f"{code.ljust(width)} // {name}" for code, name in entries]
    lines += ["", "#undef NEVERD_LINUX_SYSCALL_ARGUMENTS"]
    args.output.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
