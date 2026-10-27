#!/usr/bin/env python3
"""Extract what each C library routine returns from C headers.

The C library tables (include/neverd/libc) name each routine and the header
that declares it, but not its result type. This compiles one translation unit
that includes every such header the host provides and records, for each
routine those headers declare, whether its declared result is void, a pointer
(to an object or to a function) or another value. A routine no header
declares, such as a macro or another system's extension, is left out, so a
lookup of it stays unknown. The generated table carries no header text.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REGISTRY = ROOT / "lib" / "libc" / "LibCNames.cpp"
TABLES = ROOT / "include" / "neverd" / "libc"
OUTPUT = TABLES / "LibCReturnKinds.inc"
# Headers that declare routines the tables file under another header: glibc
# declares the malloc extensions stdlib.h lists in malloc.h.
EXTRA_HEADERS = ("malloc.h",)

REGISTER = re.compile(
    r"registerFunctions\(All, ToHeader, (k\w+Functions), (k\w+Header)\);")
HEADER = re.compile(
    r"inline constexpr std::string_view (k\w+Header) = \"([^\"]+)\";")
FUNCTIONS = re.compile(
    r"inline constexpr std::array (k\w+Functions) = \{(.*?)\};", re.S)
STRING = re.compile(r"\"([^\"]+)\"")
# A top-level declaration: `|-FunctionDecl 0x... <range> location name
# 'type' ...`.  A range spelled through a macro names `<scratch space>`, and
# no range or location holds a quote, so the name is the word before the
# first quoted type.
DECLARATION = re.compile(r"^[|`]-FunctionDecl 0x[0-9a-f]+ .*? (\w+) '([^']*)'")
# A function type returning void is spelled `void (parameters)`; `void (*`
# begins a declarator, a function returning a pointer to a function.
VOID_RESULT = re.compile(r"^void \((?!\*)")
# One returning a pointer spells a `*` before its parameters, `char *(...)`,
# or begins a declarator there, `void (*(int, ...))(int)`.
POINTER_RESULT = re.compile(r"^[^(]*(?:\*\s*\(|\(\*)")
# A top-level typedef, `|-TypedefDecl 0x... name 'type'` or, sugared,
# `'type':'canonical type'`: signal's `__sighandler_t` names a pointer.
TYPEDEF = re.compile(
    r"^[|`]-TypedefDecl 0x[0-9a-f]+ .*? (\w+) '([^']*)'(?::'([^']*)')?")


def read_tables():
    """Each C library routine the registry names, with its header."""
    sources = "".join(
        path.read_text() for path in sorted(TABLES.rglob("*.h")))
    headers = dict(HEADER.findall(sources))
    arrays = {
        name: STRING.findall(body) for name, body in FUNCTIONS.findall(sources)
    }
    routines = {}
    for functions, header in REGISTER.findall(REGISTRY.read_text()):
        if functions not in arrays or header not in headers:
            sys.exit(f"cannot read {functions} or {header}")
        for name in arrays[functions]:
            routines.setdefault(name, headers[header])
    return routines


def declared_results(compiler, headers):
    """The declared function type of each routine the headers declare."""
    unit = ["#define _GNU_SOURCE 1"]
    for header in sorted(set(headers) | set(EXTRA_HEADERS)):
        unit += [f"#if __has_include(<{header}>)", f"#include <{header}>",
                 "#endif"]
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "headers.c"
        source.write_text("\n".join(unit) + "\n")
        dump = subprocess.run(
            [compiler, "-x", "c", "-std=gnu2x", "-fsyntax-only",
             "-fno-color-diagnostics", "-Xclang", "-ast-dump", str(source)],
            check=True, capture_output=True, text=True).stdout
    types = {}
    typedefs = {}
    for line in dump.splitlines():
        match = DECLARATION.match(line)
        if match:
            types[match.group(1)] = match.group(2)
        match = TYPEDEF.match(line)
        if match:
            typedefs[match.group(1)] = match.group(3) or match.group(2)
    return types, typedefs


def result_kind(function_type, typedefs):
    """Void, Pointer or Value: what a function of this type returns."""
    if VOID_RESULT.match(function_type):
        return "Void"
    if POINTER_RESULT.match(function_type):
        return "Pointer"
    # A result a typedef names is what the typedef names.
    result = function_type.split("(", 1)[0].strip()
    seen = set()
    while result in typedefs and result not in seen:
        seen.add(result)
        result = typedefs[result]
    if result.endswith("*") or "*)" in result:
        return "Pointer"
    return "Value"


def library_version(compiler):
    macros = subprocess.run(
        [compiler, "-x", "c", "-dM", "-E", "-include", "features.h", "-"],
        check=True, capture_output=True, text=True, input="").stdout
    major = re.search(r"#define __GLIBC__ (\d+)", macros)
    minor = re.search(r"#define __GLIBC_MINOR__ (\d+)", macros)
    if not major or not minor:
        sys.exit("the headers are not the GNU C Library's")
    return f"{major.group(1)}.{minor.group(1)}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="clang")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    arguments = parser.parse_args()
    routines = read_tables()
    types, typedefs = declared_results(arguments.compiler, routines.values())
    compiler = subprocess.run([arguments.compiler, "--version"], check=True,
                              capture_output=True,
                              text=True).stdout.splitlines()[0]
    lines = [
        "// clang-format off",
        "// Generated by scripts/generate_libc_return_kinds.py.",
        f"// Declared results: GNU C Library {library_version(arguments.compiler)}"
        " headers.",
        f"// {compiler}",
        "// A routine no header declares is absent; its result stays unknown.",
    ]
    for name in sorted(routines):
        if name in types:
            kind = result_kind(types[name], typedefs)
            lines.append(f"NEVERD_LIBC_RETURN_KIND(\"{name}\", {kind})")
    arguments.output.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
