#!/usr/bin/env python3
"""Generate the UEFI type libraries NeverD ships.

A UEFI image, an application, a boot or runtime services driver, calls the
firmware through the tables it is given (the system table, boot and runtime
services) and the protocols it locates, whose types no debug information of
its own declares.  This reads the headers that declare them, EDK II's
MdePkg (https://github.com/tianocore/edk2), as a module built for each
architecture includes them, and renders the types and prototypes they
declare as the C NeverD reads back:

1. clang preprocesses what a module includes for the UEFI specification,
   the Platform Initialization specification's DXE and Management Mode
   phases, every protocol and GUID MdePkg declares, the industry standards
   (ACPI, SMBIOS, PCI and the others) and the library classes EDK II links
   into a module but those of one kind of module (UNIT_HEADERS,
   MODULE_KINDS), for the architecture's triple with
   MdePkg's Include and its architecture's ProcessorBind.h, as EDK II's
   GCC toolchain builds it.
2. The lines of the compiler's own headers are dropped, as for Linux
   (generate_linux_types.system_lines).
3. `neverd types --data-model --record-abi --keep-going --all --prototypes`
   reads what remains and renders it, with builtins spelled as C keywords.
   Declarations the reader refuses are reported, and the script fails when
   one is not of a kind expected (EXPECTED).

The headers are under the BSD-2-Clause-Patent license, which
LICENSES/edk2-headers.txt preserves.  Run it again when the headers or the
reader change.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re
import sys
import tempfile
import textwrap

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_linux_types as linux  # noqa: E402

REPOSITORY = Path(__file__).resolve().parents[1]

HEADERS = REPOSITORY / "lib" / "types" / "headers"

# What a UEFI module includes: the specifications' headers, then every
# protocol, GUID and industry standard MdePkg declares, and the library
# classes (DIRECTORIES).
UNIT_HEADERS = ("Uefi.h", "PiPei.h", "PiDxe.h", "PiMm.h")

# The directories of MdePkg/Include each of whose headers the unit includes.
DIRECTORIES = ("Protocol", "Guid", "IndustryStandard", "Library")

# The library classes a module of one kind links: each declares the entry
# point, _ModuleEntryPoint, and the routines it calls, of its kind, as an
# application's or a driver's (ImageHandle, SystemTable) and a PEIM's
# (FileHandle, PeiServices).  A program's entry takes the specification's
# EFI_IMAGE_ENTRY_POINT where its subsystem says which kind it is
# (HeaderLibraryImages.def).
MODULE_KINDS = re.compile(r"^Library/\w*EntryPoint\.h$")

# What the reader may refuse.
EXPECTED = re.compile(r"makes a type local types do not hold")


@dataclass(frozen=True)
class Target:
    triple: str
    model: str
    abi: str
    # MdePkg/Include's directory of the architecture's ProcessorBind.h.
    processor: str
    describes: str
    output: Path


TARGETS = {
    "uefi-x64": Target("x86_64-unknown-uefi", "LLP64", "microsoft", "X64",
                       "x64", HEADERS / "UefiX64.h"),
    "uefi-x86": Target("i686-unknown-windows-gnu", "ILP32", "microsoft",
                       "Ia32", "x86", HEADERS / "UefiX86.h"),
    "uefi-arm64": Target("aarch64-unknown-windows-gnu", "LLP64", "microsoft",
                         "AArch64", "ARM64", HEADERS / "UefiARM64.h"),
}


def unit_headers(include):
    """What the unit includes, in order: the specifications' headers, then
    each header of DIRECTORIES."""
    headers = list(UNIT_HEADERS)
    for directory in DIRECTORIES:
        headers += sorted(f"{directory}/{path.name}"
                          for path in (include / directory).glob("*.h"))
    return [header for header in headers if not MODULE_KINDS.match(header)]


def generate(arguments, name, target, include, commit):
    output = Path(arguments.output) if arguments.output else target.output
    resources = linux.run([arguments.clang, "-print-resource-dir"]).strip()
    clang = [arguments.clang, f"--target={target.triple}", "-nostdinc",
             "-isystem", f"{resources}/include", "-I", str(include), "-I",
             str(include / target.processor)]
    with tempfile.TemporaryDirectory(prefix="neverd-uefi-types-") as scratch:
        work = Path(scratch)
        unit = work / "module.c"
        unit.write_text("".join(f"#include <{header}>\n"
                                for header in unit_headers(include)),
                        encoding="utf-8")
        text = linux.run([*clang, "-E", str(unit)])
        defined = dict(re.findall(r"#define (\w+) (.+)",
                                  linux.run([*clang, "-E", "-dM", str(unit)])))
        source = work / "module.h"
        source.write_text(linux.system_lines(text, target, defined),
                          encoding="utf-8")
        listed = json.loads(linux.neverd_types(arguments, target, "--declare",
                                               str(source), "--keep-going",
                                               "--json"))
        problems = [problem["message"] for problem in listed["problems"]]
        unexpected = [problem for problem in problems
                      if not EXPECTED.search(problem)]
        print(f"{name}: {len(problems)} declarations refused, "
              f"{len(problems) - len(unexpected)} of them expected")
        if unexpected:
            print("\n".join(unexpected[:40]))
            sys.exit(f"{len(unexpected)} declarations refused unexpectedly")
        rendered = work / "rendered.h"
        linux.neverd_types(arguments, target, "--declare", str(source),
                           "--keep-going", "--all", "--prototypes",
                           "--spelling", "keywords", "--no-offsets", "-o",
                           str(rendered))
        body = rendered.read_text(encoding="utf-8")

    text = (
        f"The UEFI types and prototypes for {target.describes}, as a module "
        "declares them with EDK II's MdePkg headers: the UEFI and PI "
        "specifications, their protocols and GUIDs, the industry standards "
        "and the library classes.  Generated on "
        f"{linux.generation_date()} by "
        f"scripts/generate_uefi_types.py --target {name} from MdePkg at "
        f"commit {commit} of https://github.com/tianocore/edk2: changed from "
        "it, as NeverD reads and renders its declarations again.  The "
        "headers' license is in LICENSES/edk2-headers.txt.  Do not edit; "
        "generate it again.")
    header = "".join(f"// {line}\n" for line in textwrap.wrap(
        text, 77, break_long_words=False, break_on_hyphens=False))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(header + body, encoding="utf-8")
    shown = (output.relative_to(REPOSITORY)
             if output.is_relative_to(REPOSITORY) else output)
    print(f"wrote {shown}: {body.splitlines()[0].removeprefix('// ')}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", required=True,
                        help="the neverd command line tool")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--edk2", required=True,
                        help="a checkout of https://github.com/tianocore/edk2, "
                             "whose MdePkg/Include the libraries read")
    parser.add_argument("--target", action="append", choices=sorted(TARGETS),
                        help="the library to generate; repeat for more "
                             "(default: every library)")
    parser.add_argument("--output", default=None,
                        help="with one --target, where to write the library "
                             "(default: its place in lib/types/headers)")
    arguments = parser.parse_args()
    names = arguments.target or sorted(TARGETS)
    if arguments.output and len(names) != 1:
        parser.error("--output takes one --target")
    include = Path(arguments.edk2).resolve() / "MdePkg" / "Include"
    if not include.is_dir():
        parser.error(f"no MdePkg/Include in {arguments.edk2}")
    commit = linux.run(["git", "-C", arguments.edk2, "rev-parse",
                        "HEAD"]).strip()
    for name in names:
        generate(arguments, name, TARGETS[name], include, commit)


if __name__ == "__main__":
    main()
