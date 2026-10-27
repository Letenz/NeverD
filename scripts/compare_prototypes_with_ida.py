#!/usr/bin/env python3
"""Compare the signatures NeverD decompiles with the prototypes IDA shows.

IDA gives a function the prototype its type library declares for the
function's name.  NeverD completes a function its debug information names
but leaves untyped from its local types' prototypes, such as the Windows
kernel's headers.  This decompiles each function of an IDA prototype dump
(`{"functions":[{"name","prototype"},...]}`, as idalib's
`ida_typeinf.print_type` writes them) and compares the return type, the
parameters' count and names, and their types, which it resolves through
the typedefs of the header libraries NeverD ships: IDA's `PHANDLE` is
NeverD's `void**`.

    python3 scripts/compare_prototypes_with_ida.py --neverd build/bin/neverd \\
        --binary ntoskrnl.exe --pdb ntkrnlmp.pdb --ida prototypes.json
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

REPOSITORY = Path(__file__).resolve().parents[1]
LIBRARIES = REPOSITORY / "lib" / "types" / "headers"

# The C NeverD prints for Windows' builtin names.
BUILTINS = {
    "VOID": "void", "void": "void", "CHAR": "char", "char": "char",
    "CCHAR": "char", "UCHAR": "uint8_t", "BOOLEAN": "uint8_t",
    "SHORT": "int16_t", "USHORT": "uint16_t", "WCHAR": "uint16_t",
    "INT": "int32_t", "UINT": "uint32_t", "LONG": "int32_t",
    "ULONG": "uint32_t", "LONGLONG": "int64_t", "ULONGLONG": "uint64_t",
    "__int64": "int64_t", "unsigned __int64": "uint64_t", "int": "int32_t",
    "unsigned int": "uint32_t", "_DWORD": "uint32_t",
}


def typedefs():
    """name -> spelling of what it names, from the shipped header libraries."""
    names = {}
    for library in LIBRARIES.glob("*.h"):
        for line in library.read_text(encoding="utf-8").splitlines():
            if not line.startswith("typedef "):
                continue
            # Function types and pointers to them: their shape, not their
            # spelling, is what a decompiled signature keeps.
            pointer = re.search(r"\(\s*(?:__\w+\s+)?\*\s*(\w+)\)\s*\(", line)
            function = re.search(r"\b(\w+)\s*\(.*\);$", line)
            if pointer:
                names[pointer.group(1)] = "fn*"
                continue
            if function and "(" in line:
                names[function.group(1)] = "fn"
                continue
            match = re.match(r"typedef (.+?)\s*(\**)\s*(\w+);$", line)
            if match:
                names[match.group(3)] = (match.group(1) + " " +
                                         match.group(2)).strip()
    return names


def canonical(spelling, names, depth=0):
    """A type's C spelling with typedefs resolved: `PHANDLE` is `void**`."""
    spelling = re.sub(r"\b(const|volatile|struct|union|enum|__ptr64)\b", "",
                      spelling).strip()
    stars = len(spelling) - len(spelling.rstrip("* "))
    base = spelling.rstrip("* ").strip()
    pointers = spelling[len(base):].count("*")
    if base in BUILTINS:
        return BUILTINS[base] + "*" * pointers
    if base in ("fn", "fn*"):
        return base + "*" * pointers
    if depth < 16 and base in names:
        if names[base].startswith("enum "):
            return "int32_t" + "*" * pointers
        inner = canonical(names[base], names, depth + 1)
        return inner + "*" * pointers
    return base.lstrip("_") + "*" * pointers


def split_parameters(text):
    parts, depth, current = [], 0, ""
    for char in text:
        if char == "," and depth == 0:
            parts.append(current.strip())
            current = ""
            continue
        depth += char in "(<"
        depth -= char in ")>"
        current += char
    if current.strip():
        parts.append(current.strip())
    return parts


def parse(signature, name):
    """(return type, [(type, name)]) of a C declaration of \\p name."""
    head, _, rest = signature.partition(name + "(")
    if not rest:
        return None
    body = rest[:rest.rfind(")")]
    head = re.sub(r"__(stdcall|cdecl|fastcall|noreturn|usercall)\b|@<\w+>", "",
                  head).strip()
    parameters = []
    for part in split_parameters(body):
        if part in ("void", ""):
            continue
        part = re.sub(r"@<\w+>", "", part).strip()
        callback = re.match(r".*\(\s*\*\s*(\w+)\)\s*\(", part)
        if callback:
            parameters.append(("fn*", callback.group(1)))
            continue
        match = re.match(r"(.*?)\s*\b(\w+)$", part)
        if match and match.group(1):
            parameters.append((match.group(1).strip(), match.group(2)))
        else:
            parameters.append((part, ""))
    return head, parameters


def decompiled_signature(arguments, address):
    """The name and signature NeverD prints for the function at address."""
    result = subprocess.run(
        [arguments.neverd, "decompile", arguments.binary, "--pdb",
         arguments.pdb, "--func", address], capture_output=True, text=True,
        timeout=600)
    lines = result.stdout.splitlines()
    for index, line in enumerate(lines):
        if not line.startswith("/* neverd.entry:"):
            continue
        for candidate in lines[index + 1:]:
            if candidate.startswith("/*") or candidate.startswith(" *"):
                continue
            match = re.search(r"\b(\w+)\(", candidate)
            if match and candidate.rstrip().endswith("{"):
                return match.group(1), candidate.rstrip(" {")
            break
    return None, None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", required=True)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--pdb", required=True)
    parser.add_argument("--ida", required=True, help="IDA's prototype dump")
    arguments = parser.parse_args()
    names = typedefs()
    dump = json.loads(Path(arguments.ida).read_text(encoding="utf-8"))
    counts = {"same": 0, "differs": 0, "ida has none": 0, "no signature": 0}
    for function in dump["functions"]:
        name, prototype = function["name"], function.get("prototype")
        if not prototype or name not in prototype:
            counts["ida has none"] += 1
            print(f"skip {name}: IDA shows {'no' if not prototype else 'another'}"
                  " prototype")
            continue
        if "guess" in str(function.get("type_source")) or \
                "__usercall" in prototype or "__fastcall" in prototype:
            counts["ida has none"] += 1
            print(f"skip {name}: IDA guessed its prototype")
            continue
        ida = parse(prototype, name)
        named, signature = decompiled_signature(arguments, function["ea"])
        ours = parse(signature, named) if signature else None
        if not ida or not ours:
            counts["no signature"] += 1
            print(f"FAIL {name}: no signature to compare")
            continue
        problems = []
        if named != name:
            problems.append(f"named {named}")
        if canonical(ida[0], names) != canonical(ours[0], names):
            problems.append(f"returns {ours[0]}, IDA {ida[0]}")
        if len(ida[1]) != len(ours[1]):
            problems.append(f"{len(ours[1])} parameters, IDA {len(ida[1])}")
        for index, ((ida_type, ida_name), (our_type, our_name)) in enumerate(
                zip(ida[1], ours[1])):
            if ida_name != our_name:
                problems.append(f"parameter {index} is {our_name}, IDA "
                                f"{ida_name}")
            if canonical(ida_type, names) != canonical(our_type, names):
                problems.append(f"{ida_name or index} is {our_type}, IDA "
                                f"{ida_type}")
        if problems:
            counts["differs"] += 1
            print(f"DIFF {name}: " + "; ".join(problems))
        else:
            counts["same"] += 1
            print(f"same {name}")
    print(", ".join(f"{count} {what}" for what, count in counts.items()))
    return 1 if counts["no signature"] else 0


if __name__ == "__main__":
    sys.exit(main())
