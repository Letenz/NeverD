#!/usr/bin/env python3
"""Generate the Linux and Android type libraries NeverD ships.

A program's calls to the C library and the system name routines whose
parameters no debug information of its own declares.  This reads the
headers that declare them, from the GNU C Library for Linux programs and
from bionic for Android's, as a program built for each architecture
includes them, and renders the types and prototypes they declare as the C
NeverD reads back:

1. clang preprocesses what a program includes for the C library, POSIX and
   the Linux interfaces (UNIT_HEADERS), with _GNU_SOURCE as most programs
   build, for the architecture's triple: against the glibc headers
   installed for it (--sysroot), or against a checkout of bionic (--bionic)
   for Android API level 35.  glibc 2.38 and later give some routines the
   name a program built so imports (`__isoc23_strtol` for strtol); the
   reader declares each under both.
2. The lines of the compiler's own headers (<stddef.h>, <stdarg.h>) are
   dropped.  What they declare that the C library names comes first:
   size_t and ptrdiff_t as the target's predefined macros give them, and
   the 128-bit integers 64-bit targets predefine.  va_list stays the
   compiler's, which the reader takes as the address a parameter passes,
   and the C libraries' typedefs of it are dropped: the C a program
   decompiles to includes <stdarg.h> for it.  wchar_t, which C++ alone
   builds in, is written as the integer the target makes it
   (__WCHAR_TYPE__: int on x86, unsigned int on ARM).  An alignment
   attribute without its value asks the target's largest alignment, which
   is written out (__BIGGEST_ALIGNMENT__).
3. `neverd types --data-model --record-abi --keep-going --all --prototypes`
   reads what remains and renders it, with builtins spelled as C keywords.
   Declarations the reader refuses are reported, and the script fails when
   one is not of a kind expected (EXPECTED): the interchange floating types
   and vector types local types do not hold, a typedef that changes its
   type's alignment, and what names them.

The output starts with where it came from.  Run it again when the headers or
the reader change, and check the result with scripts/check_type_renders.py.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import datetime
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import textwrap

REPOSITORY = Path(__file__).resolve().parents[1]

HEADERS = REPOSITORY / "lib" / "types" / "headers"

# What a program includes for the C library, POSIX and the Linux interfaces
# glibc declares: files and directories, processes, threads and signals,
# memory, time, sockets and name lookup, dynamic loading, terminals, the
# event interfaces, locales and wide characters, and the System V IPC.
UNIT_HEADERS = (
    "stdio", "stdlib", "string", "strings", "unistd", "fcntl", "errno",
    "ctype", "wchar", "wctype", "locale", "time", "signal", "setjmp", "math",
    "fenv", "inttypes", "stdint", "limits", "assert", "dirent", "dlfcn",
    "pthread", "semaphore", "sched", "poll", "termios", "pwd", "grp", "glob",
    "fnmatch", "regex", "getopt", "syslog", "spawn", "iconv", "langinfo",
    "libgen", "link", "elf", "netdb", "ifaddrs", "search", "utime", "utmp",
    "sys/types", "sys/stat", "sys/statvfs", "sys/time", "sys/times",
    "sys/wait", "sys/mman", "sys/ioctl", "sys/socket", "sys/un", "sys/uio",
    "sys/select", "sys/epoll", "sys/eventfd", "sys/inotify", "sys/resource",
    "sys/utsname", "sys/file", "sys/sendfile", "sys/prctl", "sys/ptrace",
    "sys/syscall", "sys/sysinfo", "sys/timerfd", "sys/signalfd", "sys/xattr",
    "sys/random", "sys/auxv", "sys/sem", "sys/shm", "sys/msg", "sys/ipc",
    "netinet/in", "netinet/tcp", "arpa/inet", "net/if", "aio", "mqueue",
    "malloc", "execinfo", "err", "error", "ucontext", "fts", "ftw",
    "wordexp", "shadow")

# The headers of UNIT_HEADERS bionic does not have.
NOT_BIONIC = ("aio", "mqueue", "wordexp", "shadow")

# glibc's and bionic's typedefs of the compiler's va_list (<stdio.h>,
# <wchar.h>, <sys/cdefs.h>).
VA_LIST_TYPEDEF = re.compile(
    r"\s*typedef\s+(?:__gnuc_va_list\s+va_list|__builtin_va_list\s+__va_list)"
    r"\s*;\s*$")

# The 128-bit integers GCC and clang predefine for 64-bit targets.
WIDE_INTEGERS = """typedef __int128 __int128_t;
typedef unsigned __int128 __uint128_t;
"""

# What the reader may refuse: the interchange floating types and the vector
# types local types do not hold, a typedef that aligns its type otherwise
# (glibc's __pthread_unwind_buf_t, ARM's __jmp_buf), and what names them.
EXPECTED = re.compile(
    r"makes a type local types do not hold|"
    r"a typedef that aligns or packs its type is not a local type|"
    r"'(?:_Float128x?|__float128|_Float64x|_Float32x|__ibm128|__cfloat128|"
    r"__f128|_Complex|__SV\w+|__sv_\w+|La_\w+|__pthread_unwind_buf_t|"
    r"__jmp_buf)' names no type|"
    r"the size of __cancel_jmp_buf is unknown")


@dataclass(frozen=True)
class Target:
    triple: str
    model: str
    abi: str
    sysroot: str | None
    describes: str
    output: Path
    # Bionic's kernel headers' directory for the architecture's <asm/...>;
    # none for glibc's.
    bionic_asm: str | None = None


TARGETS = {
    "linux-x64": Target("x86_64-linux-gnu", "LP64", "itanium", None,
                        "x64 (LP64)", HEADERS / "LinuxX64.h"),
    "linux-x86": Target("i386-linux-gnu", "ILP32", "i386-sysv", None,
                        "x86 (ILP32)", HEADERS / "LinuxX86.h"),
    "linux-arm64": Target("aarch64-linux-gnu", "LP64", "itanium",
                          "/usr/aarch64-linux-gnu", "ARM64 (LP64)",
                          HEADERS / "LinuxARM64.h"),
    "linux-arm": Target("arm-linux-gnueabihf", "ILP32", "aapcs32",
                        "/usr/arm-linux-gnueabihf", "ARM (ILP32)",
                        HEADERS / "LinuxARM.h"),
    "android-x64": Target("x86_64-linux-android35", "LP64", "itanium", None,
                          "x64 (LP64)", HEADERS / "AndroidX64.h", "asm-x86"),
    "android-x86": Target("i686-linux-android35", "ILP32", "android-i386",
                          None, "x86 (ILP32)", HEADERS / "AndroidX86.h",
                          "asm-x86"),
    "android-arm64": Target("aarch64-linux-android35", "LP64", "itanium",
                            None, "ARM64 (LP64)", HEADERS / "AndroidARM64.h",
                            "asm-arm64"),
    "android-arm": Target("armv7a-linux-androideabi35", "ILP32", "aapcs32",
                          None, "ARM (ILP32)", HEADERS / "AndroidARM.h",
                          "asm-arm"),
}


def run(command, **options):
    result = subprocess.run(command, capture_output=True, text=True, **options)
    if result.returncode:
        sys.exit(f"{command[0]} failed:\n{result.stderr.strip()}")
    return result.stdout


def system_lines(text, target, defined):
    """The preprocessed lines of glibc's headers, without line markers, after
    what the compiler's headers would declare, wchar_t written as its
    integer and alignments without their value written out."""
    kept = [f"typedef {defined['__SIZE_TYPE__']} size_t;\n"
            f"typedef {defined['__PTRDIFF_TYPE__']} ptrdiff_t;\n"]
    if target.model == "LP64":
        kept.append(WIDE_INTEGERS)
    largest = defined.get("__BIGGEST_ALIGNMENT__", "16")
    keep = True
    for line in text.splitlines():
        marker = re.match(r'# \d+ "([^"]+)"', line)
        if marker:
            path = marker.group(1)
            keep = not ("/lib/clang/" in path or path.startswith("<"))
            continue
        if keep and not VA_LIST_TYPEDEF.match(line):
            line = re.sub(r"\bwchar_t\b", defined["__WCHAR_TYPE__"], line)
            kept.append(re.sub(r"\b__aligned__\s*\)\s*\)",
                               f"__aligned__ ({largest})))", line))
    return "\n".join(kept) + "\n"


def neverd_types(arguments, target, *options):
    return run([arguments.neverd, "types", "--data-model", target.model,
                "--record-abi", target.abi, *options])


def bionic_includes(clang, bionic, target):
    """The include directories a program for Android builds against: the
    compiler's own, then bionic's C library and processed kernel headers."""
    libc = Path(bionic) / "libc"
    resources = run([clang, "-print-resource-dir"]).strip()
    return ["-nostdinc", "-isystem", f"{resources}/include", "-isystem",
            str(libc / "include"), "-isystem", str(libc / "kernel" / "uapi"),
            "-isystem", str(libc / "kernel" / "uapi" / target.bionic_asm),
            "-isystem", str(libc / "kernel" / "android" / "uapi")]


def generate(arguments, name, target):
    if target.bionic_asm:
        includes = bionic_includes(arguments.clang, arguments.bionic, target)
        headers = [header for header in UNIT_HEADERS
                   if header not in NOT_BIONIC]
    else:
        sysroot = arguments.sysroot.get(name, target.sysroot)
        if sysroot and not Path(sysroot, "include").is_dir():
            sys.exit(f"{name}: no glibc headers in {sysroot}/include; give "
                     f"--sysroot {name}=<directory>")
        includes = [f"--sysroot={sysroot}"] if sysroot else []
        headers = UNIT_HEADERS
    clang = [arguments.clang, f"--target={target.triple}", *includes]
    output = Path(arguments.output) if arguments.output else target.output
    with tempfile.TemporaryDirectory(prefix="neverd-linux-types-") as scratch:
        work = Path(scratch)
        unit = work / "system.c"
        unit.write_text("#define _GNU_SOURCE 1\n" + "".join(
            f"#include <{header}.h>\n" for header in headers),
            encoding="utf-8")
        text = run([*clang, "-E", str(unit)])
        defined = dict(re.findall(r"#define (\w+) (.+)",
                                  run([*clang, "-E", "-dM", str(unit)])))
        source = work / "system.h"
        source.write_text(system_lines(text, target, defined),
                          encoding="utf-8")
        listed = json.loads(neverd_types(arguments, target, "--declare",
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
        neverd_types(arguments, target, "--declare", str(source),
                     "--keep-going", "--all", "--prototypes", "--spelling",
                     "keywords", "--no-offsets", "-o", str(rendered))
        body = rendered.read_text(encoding="utf-8")

    if target.bionic_asm:
        commit = run(["git", "-C", str(arguments.bionic), "rev-parse",
                      "HEAD"]).strip()
        text = (
            f"The Android types and prototypes for {target.describes}, as a "
            "program declares them with bionic's headers and _GNU_SOURCE, "
            "for API level 35.  Generated on "
            f"{datetime.date.today().isoformat()} by "
            f"scripts/generate_linux_types.py --target {name} from the "
            f"headers of bionic at commit {commit}: changed from them, as "
            "NeverD reads and renders their declarations again.  The "
            "headers' notices are in LICENSES/bionic-headers.txt.  Do not "
            "edit; generate it again.")
    else:
        version = f"{defined['__GLIBC__']}.{defined['__GLIBC_MINOR__']}"
        text = (
            f"The Linux types and prototypes for {target.describes}, as a "
            "program declares them with the GNU C Library's headers and "
            "_GNU_SOURCE.  Generated on "
            f"{datetime.date.today().isoformat()} by "
            f"scripts/generate_linux_types.py --target {name} from the "
            f"headers of glibc {version}: changed from them, as NeverD "
            "reads and renders their declarations again.  The headers' "
            "notices are in LICENSES/glibc-headers.txt.  Do not edit; "
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
    parser.add_argument("--sysroot", action="append", default=[],
                        metavar="TARGET=DIRECTORY",
                        help="where a target's glibc headers are, as "
                             "<DIRECTORY>/include (default: the host's for "
                             "x86 and x64, /usr/<triple> for ARM)")
    parser.add_argument("--bionic", default=None,
                        help="a checkout of https://android.googlesource.com/"
                             "platform/bionic, whose headers the Android "
                             "libraries declare")
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
    sysroots = {}
    for given in arguments.sysroot:
        name, _, directory = given.partition("=")
        if name not in TARGETS or not directory:
            parser.error(f"--sysroot takes TARGET=DIRECTORY, not {given}")
        sysroots[name] = directory
    arguments.sysroot = sysroots
    if not arguments.bionic and any(TARGETS[name].bionic_asm for name in names):
        parser.error("the Android libraries read bionic: give --bionic")
    for name in names:
        generate(arguments, name, TARGETS[name])


if __name__ == "__main__":
    main()
