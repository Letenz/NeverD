#!/usr/bin/env python3
"""Generate the Windows kernel type library NeverD ships.

The kernel's PDB names its records but no typedefs and no prototypes, so a
driver's or the kernel's functions would show no parameters a reader knows.
This reads the kernel's headers as a driver includes them -- <ntifs.h>,
which brings <ntddk.h> and <wdm.h> -- from mingw-w64, whose DDK headers come
from the ReactOS DDK and are in the public domain, and renders the types and
prototypes they declare as the C NeverD reads back:

1. clang preprocesses <ntifs.h> for the target, for Windows 10 2004
   (NTDDI_WIN10_VB) in kernel mode.
2. The lines of the compiler's own headers (intrinsics, <stddef.h>) and of
   the C runtime's are dropped: the library describes the kernel's API, and
   C++ would see the runtime's names twice.  #pragma pack arguments that
   name macros take the macros' values, which -E leaves unexpanded.
3. `neverd types --data-model --keep-going --all --prototypes` reads what
   remains and renders it.  Declarations the reader refuses, such as vector
   types, are reported; the script fails when one is not of a kind expected.

The output starts with where it came from.  Run it again when the headers or
the reader change, and check the result with scripts/check_type_renders.py.
"""
from __future__ import annotations

import argparse
import datetime
from pathlib import Path
import re
import subprocess
import sys
import tempfile

REPOSITORY = Path(__file__).resolve().parents[1]

# Per target: the clang triple, the data model and the output.
TARGETS = {
    "x64": ("x86_64-w64-mingw32", "LLP64",
            REPOSITORY / "lib" / "types" / "headers" / "WindowsKernelX64.h"),
}

# Windows 10 2004 (19041), as the kernel the library describes.
DEFINES = ["-D_KERNEL_MODE", "-DNTDDI_VERSION=0x0A000008",
           "-D_WIN32_WINNT=0x0A00"]

# Headers whose declarations are the C runtime's, not the kernel's.
RUNTIME = re.compile(
    r"/(?:corecrt[^/]*|crtdefs|_mingw[^/]*|stddef|stdarg|stdlib|stdio|string|"
    r"wchar|ctype|errno|limits|malloc|mm_malloc|vadefs|excpt|intrin|setjmp|"
    r"x86intrin|swprintf|sec_api/[^/]*|psdk_inc/[^/]*)\.(?:h|inl)$")

# What the reader may refuse: the compiler's vector and tile types, and what
# names them.
EXPECTED = re.compile(
    r"makes a type local types do not hold|"
    r"'(?:__m\d+\w*|__v\d+\w*|__bf16|_?_?tile\w*|__mmask\d+)' names no type")


def run(command, **options):
    result = subprocess.run(command, capture_output=True, text=True, **options)
    if result.returncode:
        sys.exit(f"{command[0]} failed:\n{result.stderr.strip()}")
    return result.stdout


def macros(clang, target, include, unit):
    """The object-like macros the headers define, by name."""
    found = {}
    for line in run([clang, f"--target={target}", "-E", "-dM", *include,
                     *DEFINES, str(unit)]).splitlines():
        match = re.match(r"#define (\w+) (.+)$", line)
        if match:
            found[match.group(1)] = match.group(2).strip()
    return found


def kernel_lines(text, defined):
    """The preprocessed lines of the Windows headers, without line markers,
    #pragma pack arguments expanded."""
    kept, keep = [], True
    for line in text.splitlines():
        marker = re.match(r'# \d+ "([^"]+)"', line)
        if marker:
            path = marker.group(1)
            keep = not ("/lib/clang/" in path or RUNTIME.search(path) or
                        path.startswith("<"))
            continue
        if not keep:
            continue
        pack = re.match(r"\s*#pragma pack\(([^)]*)\)", line)
        if pack:
            parts = [part.strip() for part in pack.group(1).split(",")]
            line = "#pragma pack(" + ",".join(defined.get(part, part)
                                             for part in parts) + ")"
        kept.append(line)
    return "\n".join(kept) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", required=True,
                        help="the neverd command line tool")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--mingw", default="/usr/share/mingw-w64/include",
                        help="mingw-w64's include directory")
    parser.add_argument("--target", default="x64", choices=sorted(TARGETS))
    parser.add_argument("--output", default=None,
                        help="where to write the library (default: its place "
                             "in lib/types/headers)")
    arguments = parser.parse_args()
    triple, model, output = TARGETS[arguments.target]
    output = Path(arguments.output) if arguments.output else output
    include = ["-I", f"{arguments.mingw}/ddk", "-I", arguments.mingw]
    version = run(["dpkg-query", "-W", "-f=${Version}", "mingw-w64-common"]
                  ).strip() if Path("/usr/bin/dpkg-query").exists() else "?"

    with tempfile.TemporaryDirectory(prefix="neverd-kernel-types-") as scratch:
        work = Path(scratch)
        unit = work / "kernel.c"
        unit.write_text("#include <ntifs.h>\n", encoding="utf-8")
        text = run([arguments.clang, f"--target={triple}", "-E", *include,
                    *DEFINES, str(unit)])
        source = work / "kernel.h"
        source.write_text(kernel_lines(text, macros(
            arguments.clang, triple, include, unit)), encoding="utf-8")
        rendered = work / "rendered.h"
        result = subprocess.run(
            [arguments.neverd, "types", "--data-model", model, "--declare",
             str(source), "--keep-going", "--all", "--prototypes",
             "--no-offsets", "-o", str(rendered)],
            capture_output=True, text=True)
        if result.returncode:
            sys.exit(f"neverd types failed:\n{result.stderr.strip()}")
        problems = [line for line in result.stderr.splitlines()
                    if "warning:" in line or "error:" in line]
        unexpected = [line for line in problems if not EXPECTED.search(line)]
        print(f"{len(problems)} declarations refused, "
              f"{len(problems) - len(unexpected)} of them vector types")
        if unexpected:
            print("\n".join(unexpected[:40]))
            sys.exit(f"{len(unexpected)} declarations refused unexpectedly")
        body = rendered.read_text(encoding="utf-8")

    header = (
        "// The Windows kernel's types and prototypes for "
        f"{arguments.target} ({model}), as a driver\n"
        "// declares them with <ntifs.h> for Windows 10 2004.  Generated on\n"
        f"// {datetime.date.today().isoformat()} by "
        "scripts/generate_windows_kernel_types.py from the\n"
        f"// DDK and SDK headers of mingw-w64 {version}: changed from them, as\n"
        "// NeverD reads and renders their declarations again.  The headers'\n"
        "// notices are in LICENSES/mingw-w64-headers.txt.  Do not edit;\n"
        "// generate it again.\n")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(header + body, encoding="utf-8")
    print(f"wrote {output.relative_to(REPOSITORY) if output.is_relative_to(REPOSITORY) else output}: "
          f"{body.splitlines()[0].lstrip('/ ')}")


if __name__ == "__main__":
    main()
