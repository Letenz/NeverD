#!/usr/bin/env python3
"""Generate include/neverd/libc/LibCObjectPointerParams.inc.

The C writer passes a machine integer to a header-declared function's
parameter through a cast when the parameter is an object pointer, which C
does not convert to implicitly.  This script takes the functions NeverD
declares through their headers (the registry in lib/libc/LibCNames.cpp) and
asks clang's JSON AST of those headers which parameters are object pointers.
Function pointers and va_list are not: their own rules handle them.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
REGISTRY = ROOT / "lib" / "libc" / "LibCNames.cpp"
HEADERS = ROOT / "include" / "neverd" / "libc"
OUTPUT = HEADERS / "LibCObjectPointerParams.inc"
DEFAULT_TARGET = "x86_64-linux-gnu"
# The glibc declarations, with their GNU and POSIX extensions visible.
PRELUDE = "#define _GNU_SOURCE 1\n"
# A whole translation unit of these headers dumps to tens of MiB of JSON.
MAX_AST_BYTES = 512 * 1024 * 1024
TIMEOUT_SECONDS = 600
TRAILING_QUALIFIERS = ("restrict", "__restrict", "__restrict__", "const",
                       "volatile")


def registry_pairs():
    """The (function list, header) constant names the registry registers."""
    text = REGISTRY.read_text()
    return re.findall(
        r"registerFunctions\(All, ToHeader, (k\w+Functions), (k\w+Header)\);",
        text)


def header_constants():
    """Every kXHeader name and kXFunctions list the libc headers define."""
    headers, functions = {}, {}
    for path in sorted(HEADERS.rglob("*.h")):
        text = path.read_text()
        for name, value in re.findall(
                r'inline constexpr std::string_view (k\w+Header) = "([^"]+)";',
                text):
            headers[name] = value
        for name, body in re.findall(
                r"inline constexpr std::array (k\w+Functions) = \{(.*?)\};",
                text, re.S):
            functions[name] = re.findall(r'"([^"]+)"', body)
    return headers, functions


def compiles(clang, target, header, scratch):
    source = scratch / "probe.c"
    source.write_text(f"{PRELUDE}#include <{header}>\n")
    result = subprocess.run(
        [clang, "-target", target, "-fsyntax-only", "-w", str(source)],
        capture_output=True, timeout=TIMEOUT_SECONDS)
    return result.returncode == 0


def declarations(clang, target, headers, scratch):
    """Each function declaration's parameter types, by name."""
    source = scratch / "headers.c"
    source.write_text(PRELUDE + "".join(f"#include <{h}>\n" for h in headers))
    result = subprocess.run(
        [clang, "-target", target, "-fsyntax-only", "-w", "-Xclang",
         "-ast-dump=json", str(source)],
        capture_output=True, timeout=TIMEOUT_SECONDS)
    if result.returncode != 0:
        sys.exit(result.stderr.decode(errors="replace"))
    if len(result.stdout) > MAX_AST_BYTES:
        sys.exit("the header AST is larger than expected")
    unit = json.loads(result.stdout)
    found = {}
    for decl in unit.get("inner", []):
        if decl.get("kind") != "FunctionDecl" or decl.get("isImplicit"):
            continue
        name = decl.get("name")
        params = [p.get("type", {}) for p in decl.get("inner", [])
                  if p.get("kind") == "ParmVarDecl"]
        # The first declaration with parameters speaks for every other.
        if name and name not in found and (params or "(void)" in
                                            decl.get("type", {}).get(
                                                "qualType", "")):
            found[name] = params
    return found


def object_pointer(param_type):
    spelled = param_type.get("qualType", "")
    canonical = param_type.get("desugaredQualType", spelled)
    if "va_list" in spelled or "__va_list_tag" in canonical:
        return False
    # A function or block pointer is not an object pointer.
    if "(*" in canonical or "(^" in canonical:
        return False
    words = canonical.replace("*", " * ").split()
    while words and words[-1] in TRAILING_QUALIFIERS:
        words.pop()
    return bool(words) and words[-1] in ("*", "]")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--target", default=DEFAULT_TARGET)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()

    header_names, function_lists = header_constants()
    groups = []
    for functions, header in registry_pairs():
        if functions not in function_lists or header not in header_names:
            sys.exit(f"cannot read {functions} or {header}")
        groups.append((header_names[header], function_lists[functions]))

    with tempfile.TemporaryDirectory() as directory:
        scratch = Path(directory)
        available = [h for h, _ in groups
                     if compiles(args.clang, args.target, h, scratch)]
        found = declarations(args.clang, args.target, available, scratch)

    lines = [
        "// Parameters through which standard C and POSIX functions take an",
        "// object pointer, which C does not convert a machine integer to: the",
        "// function name without leading underscores and the zero-based",
        "// parameter position.  Function pointers and va_list are not object",
        "// pointers.  Generated by scripts/generate_libc_pointer_params.py from",
        f"// the glibc headers for {args.target}; regenerate it rather than edit",
        "// it.  Included by LibCCallTraits.cpp with LIBC_OBJECT_POINTER_PARAM",
        "// defined.",
    ]
    for header, functions in groups:
        if header not in available:
            print(f"skipped <{header}>: it does not compile for {args.target}",
                  file=sys.stderr)
            continue
        entries = []
        for name in functions:
            for index, param in enumerate(found.get(name, [])):
                if object_pointer(param):
                    entries.append(
                        f'LIBC_OBJECT_POINTER_PARAM("{name}", {index})')
        if entries:
            lines += ["", f"// {header}", *entries]
    args.output.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
