#!/usr/bin/env python3
"""Generate the FreeBSD type libraries NeverD ships.

A program's calls to the C library and the system name routines whose
parameters no debug information of its own declares.  This reads the
headers that declare them, as FreeBSD installs them in /usr/include, and
renders the types and prototypes they declare as the C NeverD reads back:

1. The headers are a release's: base.txz of
   https://download.freebsd.org/releases/<architecture>/, unpacked (--base
   amd64=<directory> and arm64=<directory>, each holding usr/include).  A
   32-bit program reads the 64-bit base's headers, as FreeBSD builds its
   32-bit libraries: the machine headers that differ for it take their
   32-bit forms from usr/include/i386 in amd64's and usr/include/arm in
   arm64's.
2. clang preprocesses what a program includes for the C library, POSIX and
   FreeBSD's own interfaces (UNIT_HEADERS: kqueue, Capsicum, sysctl, jails,
   the process controls, and the libraries libutil, libkvm and libmd) for the
   architecture's triple; FreeBSD's headers declare every interface unless a
   program asks for a standard alone.  The lines of the compiler's own
   headers are dropped and each declaration follows a `#pragma
   neverd_header` naming the header of the unit it came through.  size_t and
   ptrdiff_t come first as the target's predefined macros give them, and
   the 128-bit integers 64-bit targets predefine.  va_list stays the
   compiler's, which the reader takes as the address a parameter passes, and
   FreeBSD's typedefs of it are dropped, as are its symbol versions, which
   declare nothing.  wchar_t, which C++ alone builds in, is written as the
   integer the target makes it (__WCHAR_TYPE__: int on x86, unsigned int
   on ARM).
3. `neverd types --data-model --record-abi --keep-going --all --prototypes`
   reads what remains and renders it.  Declarations the reader refuses are
   reported, and the script fails when one is not of a kind expected
   (EXPECTED).

--notices writes LICENSES/freebsd-headers.txt from the notices of the
headers the libraries read.  The output starts with where it came from.  Run
it again when the headers or the reader change.
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

REPOSITORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY / "scripts"))

import generate_darwin_types as darwin  # noqa: E402
import generate_linux_types as linux  # noqa: E402

HEADERS = REPOSITORY / "lib" / "types" / "headers"

# What a program includes for the C library, POSIX and FreeBSD's own
# interfaces: files and directories, processes, threads and signals, memory,
# time, sockets and name lookup, dynamic loading, terminals, kqueue, the event
# descriptors, Capsicum, sysctl, jails, the process and CPU set controls,
# extended attributes, the kernel linker, locales and wide characters, the
# System V IPC, and libutil's, libkvm's and libmd's routines.  sys/types and
# sys/param come first, as the system's own headers expect them.
UNIT_HEADERS = (
    "sys/types", "sys/param", "stdio", "stdlib", "string", "strings",
    "unistd", "fcntl", "errno", "ctype", "wchar", "wctype", "locale",
    "xlocale", "time", "signal", "setjmp", "math", "fenv", "inttypes",
    "stdint", "limits", "assert", "dirent", "dlfcn", "pthread", "pthread_np",
    "semaphore", "sched", "poll", "termios", "pwd", "grp", "glob", "fnmatch",
    "regex", "getopt", "syslog", "spawn", "iconv", "langinfo", "libgen",
    "link", "elf", "netdb", "ifaddrs", "search", "utime", "utmpx",
    "sys/stat", "sys/statvfs", "sys/time", "sys/times", "sys/wait",
    "sys/mman", "sys/ioctl", "sys/socket", "sys/un", "sys/uio", "sys/select",
    "sys/event", "sys/eventfd", "sys/inotify", "sys/timerfd", "sys/resource",
    "sys/utsname", "sys/file", "sys/ptrace", "sys/syscall", "sys/random",
    "sys/auxv", "sys/sem", "sys/shm", "sys/msg", "sys/ipc", "sys/sysctl",
    "sys/procctl", "sys/cpuset", "sys/capsicum", "sys/extattr", "sys/mount",
    "sys/endian", "sys/linker", "sys/module", "sys/jail", "netinet/in",
    "netinet/tcp", "arpa/inet", "net/if", "aio", "mqueue", "malloc_np",
    "execinfo", "err", "ucontext", "fts", "ftw", "wordexp", "paths", "vis",
    "libutil", "kvm", "md5", "sha256")

# FreeBSD's typedefs of the compiler's va_list (<sys/_types.h>, <stdio.h>)
# and of wchar_t, which C++ builds in and the reader takes as a keyword
# (<stddef.h>, <stdlib.h>, <wchar.h>).
DROPPED_TYPEDEF = re.compile(
    r"\s*typedef\s+(?:(?:__builtin_va_list|__va_list)\s+"
    r"(?:__va_list|va_list|__gnuc_va_list)|___wchar_t\s+wchar_t)\s*;\s*$")

# The symbol versions a header gives a routine's older forms
# (`__asm__(".symver __qsort_r_compat, qsort_r@FBSD_1.0")`), which declare
# nothing.
SYMBOL_VERSION = re.compile(r'\s*__asm__\s*\(\s*"\.symver ')

# What the reader may refuse: the interchange floating types and the vector
# types local types do not hold, thread-local objects, a typedef that aligns
# its type otherwise (the audit flags' 64-bit integer aligned to 8, which
# i386 aligns to 4), and what names them.
EXPECTED = re.compile(
    r"makes a type local types do not hold|"
    r"a typedef that aligns or packs its type is not a local type|"
    r"'(?:_Float128x?|__float128|_Float64x|_Float32x|_Complex|"
    r"_Thread_local|au_asflgs_t)' names no type")


@dataclass(frozen=True)
class Target:
    triple: str
    model: str
    abi: str
    # The base whose usr/include the program reads.
    base: str
    describes: str
    output: Path


RELEASE = "15.1"

TARGETS = {
    "freebsd-x64": Target(f"x86_64-unknown-freebsd{RELEASE}", "LP64",
                          "itanium", "amd64", "x64 (LP64)",
                          HEADERS / "FreeBSDX64.h"),
    "freebsd-x86": Target(f"i386-unknown-freebsd{RELEASE}", "ILP32",
                          "i386-sysv", "amd64", "x86 (ILP32)",
                          HEADERS / "FreeBSDX86.h"),
    "freebsd-arm64": Target(f"aarch64-unknown-freebsd{RELEASE}", "LP64",
                            "itanium", "arm64", "ARM64 (LP64)",
                            HEADERS / "FreeBSDARM64.h"),
    "freebsd-arm": Target(f"armv7-unknown-freebsd{RELEASE}-gnueabihf",
                          "ILP32", "aapcs32", "arm64", "ARM (ILP32)",
                          HEADERS / "FreeBSDARM.h"),
}


def system_lines(text, target, defined, headers):
    """The preprocessed lines of FreeBSD's headers, without line markers,
    after what the compiler's headers would declare, wchar_t written as its
    integer and alignments without their value written out; each after the
    #pragma neverd_header of the header of the unit's headers it came
    through."""
    kept = [f"typedef {defined['__SIZE_TYPE__']} size_t;\n"
            f"typedef {defined['__PTRDIFF_TYPE__']} ptrdiff_t;\n"]
    if target.model == "LP64":
        kept.append(linux.WIDE_INTEGERS)
    largest = defined.get("__BIGGEST_ALIGNMENT__", "16")
    keep = True
    reading, attributed = [], ""
    for line in text.splitlines():
        marker = re.match(r'# \d+ "([^"]+)"((?: \d)*)', line)
        if marker:
            path, flags = marker.group(1), marker.group(2).split()
            if "1" in flags or not reading:
                reading.append(path)
            elif "2" in flags:
                while len(reading) > 1 and reading[-1] != path:
                    reading.pop()
            keep = not ("/lib/clang/" in path or path.startswith("<"))
            header = (linux.unit_header(reading[1], headers)
                      if len(reading) > 1 else attributed)
            if header != attributed:
                kept.append(f'#pragma neverd_header("{header}")')
                attributed = header
            continue
        if keep and not DROPPED_TYPEDEF.match(line) and \
                not SYMBOL_VERSION.match(line):
            line = re.sub(r"\bwchar_t\b", defined["__WCHAR_TYPE__"], line)
            kept.append(re.sub(r"\b__aligned__\s*\)\s*\)",
                               f"__aligned__ ({largest})))", line))
    return "\n".join(kept) + "\n"


def generate(arguments, name, target, read):
    output = Path(arguments.output) if arguments.output else target.output
    with tempfile.TemporaryDirectory(prefix="neverd-freebsd-types-") as d:
        work = Path(d)
        tree = Path(arguments.base[target.base]).resolve() / "usr" / "include"
        resources = linux.run([arguments.clang, "-print-resource-dir"]).strip()
        clang = [arguments.clang, f"--target={target.triple}", "-nostdinc",
                 "-isystem", f"{resources}/include", "-isystem", str(tree)]
        unit = work / "system.c"
        unit.write_text("".join(f"#include <{header}.h>\n"
                                for header in UNIT_HEADERS), encoding="utf-8")
        text = linux.run([*clang, "-E", str(unit)])
        defined = dict(re.findall(r"#define (\w+) (.+)",
                                  linux.run([*clang, "-E", "-dM", str(unit)])))
        # The headers the unit read, whose notices the libraries carry.
        for path in set(re.findall(r'^# \d+ "([^"]+)"', text, re.M)):
            if path.startswith(str(tree)):
                read.add((target.base, Path(path).relative_to(tree)))
        source = work / "system.h"
        source.write_text(system_lines(text, target, defined, UNIT_HEADERS),
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

    version = defined.get("__FreeBSD__", RELEASE.split(".")[0])
    text = (
        f"The FreeBSD types and prototypes for {target.describes}, as a "
        "program declares them with FreeBSD's headers.  Generated on "
        f"{linux.generation_date()} by "
        f"scripts/generate_freebsd_types.py --target {name} from the headers "
        f"of FreeBSD {RELEASE}-RELEASE (FreeBSD {version}, base.txz of "
        "https://download.freebsd.org/releases/): changed from them, as "
        "NeverD reads and renders their declarations again.  The headers' "
        "notices are in LICENSES/freebsd-headers.txt.  Do not edit; generate "
        "it again.")
    header = "".join(f"// {line}\n" for line in textwrap.wrap(
        text, 77, break_long_words=False, break_on_hyphens=False))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(header + body, encoding="utf-8")
    shown = (output.relative_to(REPOSITORY)
             if output.is_relative_to(REPOSITORY) else output)
    print(f"wrote {shown}: {body.splitlines()[0].removeprefix('// ')}")


def write_notices(arguments, read):
    """LICENSES/freebsd-headers.txt: each notice the headers the libraries
    read open with, each license text once after the copyright lines of
    every header that states it."""
    sources = []
    for base, path in sorted(read, key=str):
        root = Path(arguments.base[base]) / "usr" / "include"
        if (root / path).is_file():
            sources.append(root / path)
    blocks = darwin.notices(sorted(set(sources)))
    intro = textwrap.fill(
        "FreeBSD headers read by scripts/generate_freebsd_types.py.  The "
        "FreeBSD type libraries, lib/types/headers/FreeBSDX64.h, "
        "FreeBSDX86.h, FreeBSDARM64.h and FreeBSDARM.h, are rendered from "
        f"the headers of FreeBSD {RELEASE}-RELEASE as its base.txz installs "
        "them in /usr/include, and hold only what NeverD reads from them and "
        "renders again: the layouts of data structures, the values of "
        "enumerations and the declarations of functions.  The notices of "
        "the headers read follow, each license once after the copyright "
        "lines of every header that states it.", 79)
    rule = "\n\n" + "-" * 79 + "\n\n"
    path = REPOSITORY / "LICENSES" / "freebsd-headers.txt"
    path.write_text(intro + rule + rule.join(blocks) + "\n", encoding="utf-8")
    print(f"wrote {path.relative_to(REPOSITORY)}: {len(blocks)} notices")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", required=True,
                        help="the neverd command line tool")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--base", action="append", default=[],
                        metavar="ARCHITECTURE=DIRECTORY",
                        help="an unpacked base.txz of FreeBSD "
                             f"{RELEASE}-RELEASE for amd64 or arm64, which "
                             "holds usr/include")
    parser.add_argument("--target", action="append", choices=sorted(TARGETS),
                        help="the library to generate; repeat for more "
                             "(default: every library)")
    parser.add_argument("--output", default=None,
                        help="with one --target, where to write the library "
                             "(default: its place in lib/types/headers)")
    parser.add_argument("--notices", action="store_true",
                        help="write LICENSES/freebsd-headers.txt too, from "
                             "the headers the libraries read")
    arguments = parser.parse_args()
    names = arguments.target or sorted(TARGETS)
    if arguments.output and len(names) != 1:
        parser.error("--output takes one --target")
    bases = {}
    for given in arguments.base:
        architecture, _, directory = given.partition("=")
        if architecture not in ("amd64", "arm64") or not directory:
            parser.error("--base takes amd64= or arm64=DIRECTORY, not "
                         f"{given}")
        if not Path(directory, "usr", "include").is_dir():
            parser.error(f"{directory}/usr/include is no directory")
        bases[architecture] = directory
    arguments.base = bases
    for name in names:
        if TARGETS[name].base not in bases:
            parser.error(f"{name} reads the {TARGETS[name].base} base: give "
                         f"--base {TARGETS[name].base}=<directory>")
    read = set()
    for name in names:
        generate(arguments, name, TARGETS[name], read)
    if arguments.notices:
        write_notices(arguments, read)


if __name__ == "__main__":
    main()
