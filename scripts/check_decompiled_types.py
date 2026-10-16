#!/usr/bin/env python3
"""Check that decompiled units hold the types they name.

Decompiles functions of a program with local types, the Windows kernel by
default, through HighC and LLVM-C, and compiles each unit for the program's
target, so that a record a unit reads members of without defining it, or a
value C does not convert to the type of the member or parameter it reaches,
fails:

    python3 scripts/check_decompiled_types.py --neverd build/bin/neverd \\
        --binary ntoskrnl.exe --pdb ntkrnlmp.pdb
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

# Kernel functions that read and write records: members of anonymous unions,
# a function pointer member, a scalar of a union in place, a header's byte
# no one member names, and a signature of records only pointed to.
FUNCTIONS = ("KeInitializeDpc", "KeSetImportanceDpc", "PsGetProcessId",
             "PsGetProcessCreateTimeQuadPart", "KeInitializeSemaphore",
             "NtCreateFile")

# What the units must not do: read a record they leave incomplete, or store
# a value C does not convert to the member it stores to.
ERRORS = ("-Werror=int-conversion", "-Werror=incompatible-pointer-types",
          "-Werror=incompatible-function-pointer-types")

# The CLI's lines before the unit.
BANNER = re.compile(r"\A(?:(?:=== NeverD|File:|Arch:|Debug:)[^\n]*\n)+")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.
                                     RawDescriptionHelpFormatter)
    parser.add_argument("--neverd", required=True,
                        help="the neverd command line tool")
    parser.add_argument("--binary", required=True,
                        help="the program to decompile")
    parser.add_argument("--pdb", default=None,
                        help="its PDB, when the program does not find it")
    parser.add_argument("--func", action="append", default=None,
                        help="a function to decompile; repeat for more "
                             "(default: the kernel's list)")
    parser.add_argument("--route", action="append", default=None,
                        choices=("highc", "llvmc"),
                        help="the route to decompile through; repeat for "
                             "both (default both)")
    parser.add_argument("--clang", default="clang",
                        help="the clang driver the units compile with")
    parser.add_argument("--target", default="x86_64-w64-windows-gnu",
                        help="the target the units compile for")
    parser.add_argument("--sysroot-include", default=None,
                        help="headers for the target, such as mingw-w64's "
                             "/usr/x86_64-w64-mingw32/include")
    parser.add_argument("--keep", default=None,
                        help="keep the units in this directory")
    arguments = parser.parse_args()
    clang = shutil.which(arguments.clang)
    if not clang:
        sys.exit(f"{arguments.clang} not found")
    include = arguments.sysroot_include
    if include is None and pathlib.Path(
            "/usr/x86_64-w64-mingw32/include").is_dir():
        include = "/usr/x86_64-w64-mingw32/include"
    if arguments.keep:
        work = pathlib.Path(arguments.keep)
        work.mkdir(parents=True, exist_ok=True)
        check(arguments, clang, include, work)
        return
    with tempfile.TemporaryDirectory(prefix="neverd-decompiled-") as work:
        check(arguments, clang, include, pathlib.Path(work))


def check(arguments, clang, include, work):
    """Decompiles each function into a unit in work and compiles it."""
    failures = 0
    for function in arguments.func or FUNCTIONS:
        for route in arguments.route or ("highc", "llvmc"):
            what = f"{function} {route}"
            command = [arguments.neverd, "decompile", arguments.binary,
                       "--func", function]
            if arguments.pdb:
                command += ["--pdb", arguments.pdb]
            if route == "llvmc":
                command.append("--llvm")
            result = subprocess.run(command, capture_output=True, text=True)
            unit = work / f"{function}-{route}.c"
            unit.write_text(BANNER.sub("", result.stdout), encoding="utf-8")
            if result.returncode:
                failures += 1
                print(f"FAIL {what}: neverd: {result.stderr.strip()[:200]}")
                continue
            compile_command = [clang, f"--target={arguments.target}",
                               "-fsyntax-only", "-std=c11", *ERRORS]
            if include:
                compile_command += ["-isystem", include]
            compiled = subprocess.run(compile_command + ["-x", "c", str(unit)],
                                      capture_output=True, text=True)
            errors = [line for line in compiled.stderr.splitlines()
                      if "error:" in line]
            if compiled.returncode:
                failures += 1
                print(f"FAIL {what}")
                for line in errors[:10]:
                    print(f"     {line}")
            else:
                print(f"ok   {what}")
    if failures:
        sys.exit(f"{failures} unit(s) failed")
    print("every unit compiled")


if __name__ == "__main__":
    main()
