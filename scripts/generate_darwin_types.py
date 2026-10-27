#!/usr/bin/env python3
"""Generate the Darwin type libraries NeverD ships.

A macOS or iOS program's calls to libSystem, the C library, the BSD and Mach
interfaces, threads and the dynamic linker, name routines whose parameters
no debug information of its own declares.  This reads the headers whose
source Apple publishes (https://github.com/apple-oss-distributions), as the
projects' builds install them for each platform, and renders the types and
prototypes they declare as the C NeverD reads back:

1. The headers each project installs (INSTALLS) are copied into one include
   tree, laid out as an SDK's usr/include holds them, without the blocks
   Libc's install removes (`//Begin-Libc`).  What xnu's build generates is
   generated the same way: AvailabilityVersions' availability tool renders
   the Availability headers, and xnu's scripts sys/_symbol_aliasing.h and
   sys/_posix_availability.h with it.  clang predefines the TARGET_OS_
   macros <TargetConditionals.h> would, which the tree's leaves empty.
2. clang preprocesses what a program includes for the C library, POSIX and
   the Darwin interfaces libSystem declares (UNIT_HEADERS), for the
   platform's triple, as a program does by default: Darwin's headers then
   declare everything, and name routines as such a program imports them
   (_DARWIN_C_SOURCE would name fopen fopen$DARWIN_EXTSN).  The conditions the
   projects' installs resolve with unifdef are given as macros instead:
   Libc's features for the platform (its generate_features.pl --unifdef),
   xnu's platform and public headers (XNU_UNIFDEF).  Blocks are off: a block
   pointer is no type local types hold.
3. The lines of clang's own headers are dropped, as for Linux
   (generate_linux_types.system_lines), with the typedef the headers give
   wchar_t, which is the compiler's, and each declaration follows the
   `#pragma neverd_header` of the header the unit included it through.  An assembler name loses the underscore
   Mach-O gives every C name, so that a routine takes its prototype under
   the name a program imports it by (`stat$INODE64` on x64).
4. `neverd types --data-model --record-abi --keep-going --all --prototypes`
   reads what remains and renders it, with builtins spelled as C keywords.
   Declarations the reader refuses are reported, and the script fails when
   one is not of a kind expected (EXPECTED).

The headers are under the Apple Public Source License 2.0, and many under
the BSD licenses of the code they take from FreeBSD and NetBSD.  Each
generated library carries APSL 2.0's notice, and LICENSES/darwin-headers.txt
the license and the notices of every header the libraries read, which this
gathers.  Run it again when the headers or the reader change.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_linux_types as linux  # noqa: E402

REPOSITORY = Path(__file__).resolve().parents[1]

HEADERS = REPOSITORY / "lib" / "types" / "headers"

NOTICES = REPOSITORY / "LICENSES" / "darwin-headers.txt"

# The projects, by the tag of each the libraries were generated from, under
# https://github.com/apple-oss-distributions/<project>.
PROJECTS = ("Libc", "xnu", "libpthread", "Libinfo", "libmalloc",
            "libplatform", "dyld", "AvailabilityVersions")

# Where each project installs the public headers a program includes, as an
# SDK's usr/include holds them: (project, source, destination).  A later
# row replaces what an earlier one copied: libplatform's <libkern/OSAtomic.h>
# is a program's, xnu's the kernel's.
INSTALLS = (
    ("Libc", "include", ""),
    ("Libc", "include/NetBSD/utmpx.h", "utmpx.h"),
    ("Libc", "include/FreeBSD/nl_types.h", "nl_types.h"),
    ("Libc", "stdtime/FreeBSD/tzfile.h", "tzfile.h"),
    ("Libc", "gen/execinfo.h", "execinfo.h"),
    ("Libc", "gen/get_compat.h", "get_compat.h"),
    ("xnu", "bsd/sys", "sys"),
    ("xnu", "bsd/machine", "machine"),
    ("xnu", "bsd/i386", "i386"),
    ("xnu", "bsd/arm", "arm"),
    ("xnu", "osfmk/i386/eflags.h", "i386/eflags.h"),
    ("xnu", "osfmk/arm/arch.h", "arm/arch.h"),
    ("xnu", "bsd/net", "net"),
    ("xnu", "bsd/netinet", "netinet"),
    ("xnu", "bsd/netinet6", "netinet6"),
    ("xnu", "bsd/bsm", "bsm"),
    ("xnu", "bsd/uuid", "uuid"),
    ("xnu", "osfmk/mach", "mach"),
    ("xnu", "osfmk/mach_debug", "mach_debug"),
    ("xnu", "libsyscall/mach/mach", "mach"),
    ("xnu", "libkern/libkern", "libkern"),
    ("xnu", "libkern/os", "os"),
    ("xnu", "libsyscall/wrappers/gethostuuid.h", "gethostuuid.h"),
    ("xnu", "libsyscall/wrappers/spawn/spawn.h", "spawn.h"),
    ("xnu", "libsyscall/wrappers/libproc/libproc.h", "libproc.h"),
    ("xnu", "EXTERNAL_HEADERS/architecture", "architecture"),
    ("xnu", "EXTERNAL_HEADERS/mach-o", "mach-o"),
    ("xnu", "EXTERNAL_HEADERS/ptrcheck.h", "ptrcheck.h"),
    ("libpthread", "include/pthread", "pthread"),
    ("libpthread", "include/pthread/pthread.h", "pthread.h"),
    ("libpthread", "include/pthread/sched.h", "sched.h"),
    ("libpthread", "include/sys", "sys"),
    ("Libinfo", "lookup.subproj/netdb.h", "netdb.h"),
    ("Libinfo", "lookup.subproj/pwd.h", "pwd.h"),
    ("Libinfo", "lookup.subproj/grp.h", "grp.h"),
    ("Libinfo", "gen.subproj/ifaddrs.h", "ifaddrs.h"),
    ("libmalloc", "include/malloc", "malloc"),
    ("libplatform", "include/setjmp.h", "setjmp.h"),
    ("libplatform", "include/ucontext.h", "ucontext.h"),
    ("libplatform", "include/os", "os"),
    ("libplatform", "include/libkern", "libkern"),
    ("dyld", "include/dlfcn.h", "dlfcn.h"),
    ("dyld", "include/mach-o", "mach-o"),
)

# The Availability headers AvailabilityVersions renders from its templates,
# and where.
AVAILABILITY = (
    ("Availability.h", "Availability.h"),
    ("AvailabilityInternal.h", "AvailabilityInternal.h"),
    ("AvailabilityInternalLegacy.h", "AvailabilityInternalLegacy.h"),
    ("AvailabilityMacros.h", "AvailabilityMacros.h"),
    ("AvailabilityVersions.h", "AvailabilityVersions.h"),
    ("os_availability.h", "os/availability.h"),
)

# What a program includes for the C library, POSIX and the Darwin interfaces
# libSystem declares: the units of the Linux libraries without the Linux
# interfaces, and the event queue, sysctl, the malloc zones, the dynamic
# linker's images, the unfair lock, the atomics and Mach's time.  <math.h>
# and <fenv.h> are left out: libm's headers are not published for ARM64.
UNIT_HEADERS = (
    "stdio", "stdlib", "string", "strings", "unistd", "fcntl", "errno",
    "ctype", "wchar", "wctype", "locale", "xlocale", "time", "signal",
    "setjmp", "inttypes", "stdint", "limits", "assert", "dirent", "dlfcn",
    "pthread", "semaphore", "sched", "poll", "termios", "pwd", "grp", "glob",
    "fnmatch", "regex", "getopt", "syslog", "spawn", "langinfo", "libgen",
    "netdb", "ifaddrs", "search", "utime", "utmpx", "sys/types", "sys/stat",
    "sys/statvfs", "sys/time", "sys/times", "sys/wait", "sys/mman",
    "sys/ioctl", "sys/socket", "sys/un", "sys/uio", "sys/select",
    "sys/event", "sys/resource", "sys/utsname", "sys/file", "sys/ptrace",
    "sys/sysctl", "sys/xattr", "sys/sem", "sys/shm", "sys/msg", "sys/ipc",
    "sys/mount", "sys/param", "sys/ucontext", "netinet/in", "netinet/tcp",
    "arpa/inet", "net/if", "aio", "malloc/malloc", "execinfo", "err", "fts",
    "ftw", "wordexp", "libproc", "mach-o/dyld", "os/lock", "libkern/OSAtomic",
    "crt_externs", "mach/mach_time")

# xnu's public headers as its install's unifdef leaves them
# (SINCFRAME_UNIFDEF, makedefs/MakeInc.def), beside the platform's
# XNU_PLATFORM_ macro; dyld's <mach-o/dyld.h> marks availability with a
# macro its tree does not define.
XNU_UNIFDEF = ("-DMODULES_SUPPORTED", "-D_OPEN_SOURCE_", "-D__OPEN_SOURCE__",
               "-DDYLD_EXCLAVEKIT_UNAVAILABLE=")

# The typedefs of the compiler's va_list and wchar_t the headers make.
COMPILER_TYPEDEF = re.compile(
    r"\s*typedef\s+(?:__builtin_va_list\s+__darwin_va_list|"
    r"__darwin_va_list\s+va_list|__darwin_wchar_t\s+wchar_t)\s*;\s*$")

# What the reader may refuse.
EXPECTED = re.compile(r"makes a type local types do not hold")


@dataclass(frozen=True)
class Target:
    triple: str
    model: str
    abi: str
    # Libc's platform (Platforms/<platform>) and xnu's.
    libc_platform: str
    xnu_platform: str
    arch: str
    describes: str
    output: Path


TARGETS = {
    "macos-x64": Target("x86_64-apple-macos15.0", "LP64", "itanium", "macosx",
                        "MacOSX", "x86_64", "macOS on x64",
                        HEADERS / "MacOSX64.h"),
    "macos-arm64": Target("arm64-apple-macos15.0", "LP64", "apple-arm64",
                          "macosx", "MacOSX", "arm64", "macOS on ARM64",
                          HEADERS / "MacOSARM64.h"),
    "ios-arm64": Target("arm64-apple-ios18.0", "LP64", "apple-arm64",
                        "iphoneos", "iPhoneOS", "arm64", "iOS on ARM64",
                        HEADERS / "IOSARM64.h"),
}


def run(command, **options):
    return linux.run(command, **options)


def copy(source, destination):
    if source.is_dir():
        shutil.copytree(source, destination, dirs_exist_ok=True)
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)


def install(checkouts, root):
    """Lays the projects' public headers out under \\p root, as their builds
    install them, and generates what xnu's build does."""
    for project, source, destination in INSTALLS:
        copy(checkouts[project] / source, root / destination)
    # Libc's install deletes the blocks only its own build reads.
    for header in root.rglob("*.h"):
        text = header.read_text(encoding="utf-8", errors="surrogateescape")
        if "//Begin-Libc" in text:
            header.write_text(re.sub(r"(?ms)^//Begin-Libc$.*?^//End-Libc$\n?",
                                     "", text),
                              encoding="utf-8", errors="surrogateescape")
    # AvailabilityVersions' tool, with its versions built in, renders its
    # templates; xnu's scripts find it in an SDK's usr/local/libexec.
    versions = checkouts["AvailabilityVersions"]
    tools = root.parent / "usr" / "local" / "libexec"
    tools.mkdir(parents=True)
    tool = tools / "availability"
    run([sys.executable, str(versions / "availability"), "--preprocess",
         str(versions / "availability"), str(tool)])
    tool.chmod(0o755)
    (tools / "availability.pl").write_text(
        f"#!/bin/sh\nexec {sys.executable} {tool} \"$@\"\n")
    (tools / "availability.pl").chmod(0o755)
    for template, destination in AVAILABILITY:
        (root / destination).parent.mkdir(parents=True, exist_ok=True)
        run([sys.executable, str(tool), "--preprocess",
             str(versions / "templates" / template), str(root / destination)])
    xnu = checkouts["xnu"] / "bsd" / "sys"
    run(["bash", str(xnu / "make_symbol_aliasing.sh"), str(root.parent),
         str(root / "sys" / "_symbol_aliasing.h")])
    run(["bash", str(xnu / "make_posix_availability.sh"),
         str(root / "sys" / "_posix_availability.h")])
    (root / "TargetConditionals.h").write_text(
        "/* clang predefines the TARGET_OS_ macros for Darwin targets. */\n")


def libc_features(checkouts, target):
    """The unifdef flags Libc's install gives the platform's headers, as
    macros."""
    environment = dict(os.environ, SRCROOT=str(checkouts["Libc"]),
                       ARCHS=target.arch,
                       VARIANT_PLATFORM_NAME=target.libc_platform,
                       DERIVED_FILES_DIR=tempfile.gettempdir())
    return run(["perl", str(checkouts["Libc"] / "xcodescripts" /
                            "generate_features.pl"), "--unifdef"],
               env=environment).split()


def clang_options(clang, checkouts, target, root):
    """What clang takes to read the headers installed under \\p root as a
    program for \\p target includes them."""
    resources = run([clang, "-print-resource-dir"]).strip()
    return [f"--target={target.triple}", "-fno-blocks", "-nostdinc",
            "-isystem", f"{resources}/include", "-isystem", str(root),
            *libc_features(checkouts, target),
            f"-DXNU_PLATFORM_{target.xnu_platform}", *XNU_UNIFDEF]


def system_lines(text, target, defined):
    lines = [line for line in text.splitlines()
             if not COMPILER_TYPEDEF.match(line)]
    kept = linux.system_lines("\n".join(lines), target, defined,
                              UNIT_HEADERS)
    # Mach-O gives every C name one underscore, which a program's import
    # names do not hold.
    return kept.replace('__asm("_" "', '__asm("')


def license_block(path):
    """The comment that opens \\p path, where it states a copyright or a
    license, without its comment marks."""
    text = path.read_text(encoding="utf-8", errors="replace")
    match = re.match(r"\s*/\*(.*?)\*/", text, re.S)
    if match:
        lines = [re.sub(r"^\s*\*? ?", "", line).rstrip()
                 for line in match.group(1).splitlines()]
    else:
        lines = []
        for line in text.splitlines():
            if not line.startswith("//"):
                break
            lines.append(line[2:].strip())
    block = "\n".join(lines).strip("\n")
    if not re.search(r"copyright|license|licence", block, re.I):
        return None
    return block


def notices(sources):
    """The notices the headers \\p sources open with, each license text once
    after the copyright lines of every header that states it."""
    grouped = {}
    for path in sorted(sources):
        block = license_block(path)
        if not block:
            continue
        lines = [line for line in block.splitlines()
                 if not re.search(r"@\(#\)|\$FreeBSD|\$NetBSD|\$OpenBSD|"
                                  r"@APPLE_\w*LICENSE_HEADER_(START|END)@",
                                  line)]
        # The copyright lines, up to the first blank line, and the text.
        at = next((i for i, line in enumerate(lines) if not line.strip()),
                  len(lines))
        head = [line.strip() for line in lines[:at]]
        if not any(re.search(r"copyright", line, re.I) for line in head):
            head, at = [], 0
        body = "\n".join(lines[at:]).strip("\n")
        rights = grouped.setdefault(body, [])
        for line in head:
            if line and line not in rights:
                rights.append(line)
    return ["\n".join(rights) + ("\n\n" if rights else "") + body
            for body, rights in grouped.items()]


def generate(arguments, name, target, checkouts, read):
    output = Path(arguments.output) if arguments.output else target.output
    with tempfile.TemporaryDirectory(prefix="neverd-darwin-types-") as scratch:
        work = Path(scratch)
        root = work / "sdk" / "include"
        install(checkouts, root)
        clang = [arguments.clang, *clang_options(arguments.clang, checkouts,
                                                 target, root)]
        unit = work / "system.c"
        unit.write_text("".join(f"#include <{header}.h>\n"
                                for header in UNIT_HEADERS),
                        encoding="utf-8")
        text = run([*clang, "-E", str(unit)])
        defined = dict(re.findall(r"#define (\w+) (.+)",
                                  run([*clang, "-E", "-dM", str(unit)])))
        # The headers the unit read, whose notices the libraries carry.
        for path in set(re.findall(r'^# \d+ "([^"]+)"', text, re.M)):
            if path.startswith(str(root)):
                read.add(Path(path).relative_to(root))
        source = work / "system.h"
        source.write_text(system_lines(text, target, defined),
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
                           "--spelling", "keywords", "--no-offsets",
                           "--header-pragmas", "-o", str(rendered))
        body = rendered.read_text(encoding="utf-8")

    tags = ", ".join(tag for project, tag in sorted(arguments.tags.items()))
    text = (
        f"The Darwin types and prototypes for {target.describes}, as a "
        "program declares them with libSystem's headers.  Generated on "
        f"{linux.generation_date()} by "
        f"scripts/generate_darwin_types.py --target {name} from the headers "
        f"of {tags} (https://github.com/apple-oss-distributions): changed "
        "from them, as NeverD reads and renders their declarations again.")
    exhibit = (
        "This file contains Original Code and/or Modifications of Original "
        "Code as defined in and that are subject to the Apple Public Source "
        "License Version 2.0 (the 'License'). You may not use this file "
        "except in compliance with the License. Please obtain a copy of the "
        "License at http://www.opensource.apple.com/apsl/ and read it before "
        "using this file.  The Original Code and all software distributed "
        "under the License are distributed on an 'AS IS' basis, WITHOUT "
        "WARRANTY OF ANY KIND, EITHER EXPRESS OR IMPLIED, AND APPLE HEREBY "
        "DISCLAIMS ALL SUCH WARRANTIES, INCLUDING WITHOUT LIMITATION, ANY "
        "WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, "
        "QUIET ENJOYMENT OR NON-INFRINGEMENT. Please see the License for "
        "the specific language governing rights and limitations under the "
        "License.  The License and the notices of the headers it was read "
        "from are in LICENSES/darwin-headers.txt.  Do not edit; generate it "
        "again.")
    header = "".join(
        f"// {line}\n" for paragraph in (text, exhibit)
        for line in textwrap.wrap(paragraph, 77, break_long_words=False,
                                  break_on_hyphens=False) + [""])
    header = header.replace("// \n", "//\n")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(header + body, encoding="utf-8")
    shown = (output.relative_to(REPOSITORY)
             if output.is_relative_to(REPOSITORY) else output)
    print(f"wrote {shown}: {body.splitlines()[0].removeprefix('// ')}")


def write_notices(checkouts, read, tags):
    """LICENSES/darwin-headers.txt: APSL 2.0, and each notice the headers
    the libraries read open with."""
    sources = []
    with tempfile.TemporaryDirectory(prefix="neverd-darwin-notices-") as d:
        root = Path(d) / "sdk" / "include"
        install(checkouts, root)
        sources = [root / path for path in read if (root / path).is_file()]
        blocks = notices(sources)
    license = (checkouts["xnu"] / "APPLE_LICENSE").read_text(encoding="utf-8")
    intro = textwrap.fill(
        "Darwin headers read by scripts/generate_darwin_types.py.  The "
        "Darwin type libraries, lib/types/headers/MacOSX64.h, MacOSARM64.h "
        "and IOSARM64.h, are rendered from the headers of "
        f"{', '.join(tag for project, tag in sorted(tags.items()))} "
        "(https://github.com/apple-oss-distributions), as each project's "
        "build installs them, and hold only what NeverD reads from them and "
        "renders again: the layouts of data structures, the values of "
        "enumerations and the declarations of functions.  They are "
        "Modifications of Original Code under the Apple Public Source "
        "License 2.0, which follows; the headers are under it, and some "
        "under the licenses of the code Apple takes from FreeBSD, NetBSD "
        "and others, whose notices follow it, each text once after the "
        "copyright lines of the headers that state it.  A few headers name "
        "an earlier version of the License, whose Original Code its "
        "versions clause lets anyone use under any later version Apple "
        "publishes, as it has 2.0.", 74, break_long_words=False,
        break_on_hyphens=False)
    rule = "=" * 74
    text = (f"{intro}\n\n{rule}\nApple Public Source License 2.0\n{rule}\n\n"
            f"{license.strip()}\n\n{rule}\nThe headers' notices\n{rule}\n")
    for block in blocks:
        text += f"\n{block}\n\n{'-' * 74}\n"
    NOTICES.write_text(text, encoding="utf-8")
    print(f"wrote {NOTICES.relative_to(REPOSITORY)}: {len(blocks)} notices")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", required=True,
                        help="the neverd command line tool")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--darwin", required=True,
                        help="a directory holding a checkout of each project "
                             "(PROJECTS) of https://github.com/"
                             "apple-oss-distributions, at the tag to read")
    parser.add_argument("--target", action="append", choices=sorted(TARGETS),
                        help="the library to generate; repeat for more "
                             "(default: every library)")
    parser.add_argument("--output", default=None,
                        help="with one --target, where to write the library "
                             "(default: its place in lib/types/headers)")
    parser.add_argument("--install-headers", default=None,
                        metavar="DIRECTORY",
                        help="only install the headers under DIRECTORY, as "
                             "the libraries read them, and print what clang "
                             "takes to read them for each target, as a "
                             "test's program is built")
    arguments = parser.parse_args()
    names = arguments.target or sorted(TARGETS)
    if arguments.output and len(names) != 1:
        parser.error("--output takes one --target")
    checkouts = {project: Path(arguments.darwin).resolve() / project
                 for project in PROJECTS}
    for project, checkout in checkouts.items():
        if not checkout.is_dir():
            parser.error(f"no checkout of {project} in {arguments.darwin}")
    if arguments.install_headers:
        sdk = Path(arguments.install_headers).resolve() / "sdk"
        shutil.rmtree(sdk, ignore_errors=True)
        root = sdk / "include"
        install(checkouts, root)
        for name in names:
            print(f"{name}: " + " ".join(clang_options(
                arguments.clang, checkouts, TARGETS[name], root)))
        return
    arguments.tags = {project: run(["git", "-C", str(checkout), "describe",
                                    "--tags", "--exact-match"]).strip()
                      for project, checkout in checkouts.items()}
    read = set()
    for name in names:
        generate(arguments, name, TARGETS[name], checkouts, read)
    if not arguments.output:
        write_notices(checkouts, read, arguments.tags)


if __name__ == "__main__":
    main()
