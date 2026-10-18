#!/usr/bin/env python3
"""Compile a program's local types as NeverD renders them.

`neverd types --all --asserts` renders every local type with compile-time
checks of each size and offset.  This script renders them in C and C++ with
every builtin spelling, in Rust, Go, Python, JavaScript and Lua with both
of their own, and compiles each with clang, rustc and go, or runs it with
python3, node or Lua 5.3+, tsc checking JavaScript's JSDoc too, so that a
layout NeverD gets wrong, or source that does not compile, fails.  The C header, which the workbench shows as
C/C++, compiles as C++ as well.  Without --binary it checks the fixture PDB
the unit tests use; with --binary, a program's own types, from its PDB or
its DWARF; with --declare, the types C, C++, Rust or Go files declare, laid
out for --data-model, such as a type library NeverD ships, and the
prototypes they declare too.  A toolchain that is missing is reported and
skipped.

A program's types render for its own target, which is often not the host's:
the toolchains scripts/fetch_render_toolchains.py fetches (--toolchains,
default ~/.cache/neverd-render-toolchains) compile Rust for that target and
run Python on one of its kind, i386 Linux's, 32-bit ARM's under qemu, or
32-bit Windows' under Wine.  Without them those checks are skipped.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

REPOSITORY = Path(__file__).resolve().parents[1]
FIXTURES = REPOSITORY / "unittests" / "types" / "fixtures"
SPELLINGS = ("portable", "windows", "keywords")
# Rust and Go name builtins by exact widths, or as Windows does, through
# aliases.
RUST_SPELLINGS = GO_SPELLINGS = PYTHON_SPELLINGS = JAVASCRIPT_SPELLINGS = \
    LUA_SPELLINGS = ("portable", "windows")

# Windows' own typedefs of the builtins it names, as <ntdef.h> declares them:
# prototypes of the compiler's intrinsics only match its declarations with
# these, where the rendered prelude gives exact widths instead.
WINDOWS_TYPEDEFS = """#define _NTDEF_
#define VOID void
typedef char CHAR;
typedef signed char SCHAR;
typedef unsigned char UCHAR;
typedef unsigned short WCHAR;
typedef short SHORT;
typedef unsigned short USHORT;
typedef int INT;
typedef unsigned int UINT;
typedef long LONG;
typedef unsigned long ULONG;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef float FLOAT;
typedef double DOUBLE;
#define _HRESULT_DEFINED
typedef long HRESULT;
"""

# Per data model: the target every spelling of C and C++ compiles for, the
# GNU target the portable spelling also compiles for, if another, Go's GOOS
# and GOARCH, the target Rust compiles for (the host's where none), and the
# Python whose ctypes lay records out as the program's do: the host's, or
# one of the toolchains (i386, armv7, win32).  A program whose record ABI
# its data model does not say, a 32-bit one GCC or Clang laid out, Apple's
# 64-bit ARM or Android's i386, goes by that ABI instead, and builds for its
# own target alone.
TARGETS = {
    "LLP64": ("x86_64-pc-windows-msvc", "x86_64-linux-gnu", "windows",
              "amd64", "x86_64-pc-windows-msvc", "host"),
    # MinGW's records, Microsoft's with the x87's long double, which MSVC's
    # compiler, whose long double is a double, then checks the portable
    # spelling of too.
    "mingw-x64": ("x86_64-w64-mingw32", "x86_64-pc-windows-msvc", "windows",
                  "amd64", "x86_64-pc-windows-gnu", "host"),
    "mingw-x86": ("i686-w64-mingw32", "i686-pc-windows-msvc", "windows",
                  "386", "i686-pc-windows-gnu", "win32"),
    "ILP32": ("i686-pc-windows-msvc", "i686-linux-gnu", "windows", "386",
              "i686-pc-windows-msvc", "win32"),
    "LP64": ("x86_64-linux-gnu", "x86_64-linux-gnu", "windows", "amd64",
             None, "host"),
    "apple-arm64": ("arm64-apple-macos", None, "darwin", "arm64",
                    "aarch64-apple-darwin", "host"),
    "i386-sysv": ("i686-linux-gnu", None, "linux", "386",
                  "i686-unknown-linux-gnu", "i386"),
    # Go builds for android/386 only through cgo; its layouts are 386's.
    # Android's i386 lays records out as i386 System V does, with a long
    # double that is a double, which ctypes then reads as c_double.
    "android-i386": ("i686-linux-android", None, "linux", "386",
                     "i686-linux-android", "i386"),
    "aapcs32": ("armv7-linux-gnueabihf", None, "linux", "arm",
                "armv7-unknown-linux-gnueabihf", "armv7"),
}
# The other compiler of each Windows target, which reads the same Windows
# headers with a long double of its own.
OTHER_WINDOWS_COMPILER = {
    "x86_64-pc-windows-msvc": "x86_64-w64-mingw32",
    "i686-pc-windows-msvc": "i686-w64-mingw32",
    "x86_64-w64-mingw32": "x86_64-pc-windows-msvc",
    "i686-w64-mingw32": "i686-pc-windows-msvc",
}
FETCH = "run scripts/fetch_render_toolchains.py"

# Each library of a system's headers (--system-library), by the compiler
# and target its own system's headers are read with: clang, against the
# toolchains' sysroot or bionic where the host has none, or zig, whose
# headers are musl's.  Its C library's types come from them there.
SYSTEMS = {
    "linux-x64": ("clang", "x86_64-linux-gnu", None),
    "linux-x86": ("clang", "i686-linux-gnu", None),
    "linux-arm64": ("clang", "aarch64-linux-gnu", "arm64"),
    "linux-arm": ("clang", "armv7-linux-gnueabihf", "armhf"),
    "android-x64": ("clang", "x86_64-linux-android", "asm-x86"),
    "android-x86": ("clang", "i686-linux-android", "asm-x86"),
    "android-arm64": ("clang", "aarch64-linux-android", "asm-arm64"),
    "android-arm": ("clang", "armv7a-linux-androideabi", "asm-arm"),
    "musl-x64": ("zig", "x86_64-linux-musl", None),
    "musl-x86": ("zig", "x86-linux-musl", None),
    "musl-arm64": ("zig", "aarch64-linux-musl", None),
    "musl-arm": ("zig", "arm-linux-musleabihf", None),
    # Apple's headers as the Darwin libraries read them (the toolchains'
    # darwin-sdk), whose clang options name the target.
    "macos-x64": ("clang", "x86_64-apple-macos", "darwin:macos-x64"),
    "macos-arm64": ("clang", "arm64-apple-macos", "darwin:macos-arm64"),
    "ios-arm64": ("clang", "arm64-apple-ios", "darwin:ios-arm64"),
}
# zig's target of the system a library is of, whose headers zig holds at
# another version than the library read: no other system's.
SAME_SYSTEM = {"macos-x64": "x86_64-macos", "macos-arm64": "aarch64-macos"}
# The other systems of each pointer width a library's portable C compiles
# for, defining its C library's types itself: Windows' two compilers,
# macOS's, and Linux's on the other C libraries.
OTHER_SYSTEMS = {
    8: (("clang", "x86_64-w64-mingw32", None),
        ("clang", "x86_64-pc-windows-msvc", None),
        ("zig", "aarch64-macos", None), ("zig", "x86_64-macos", None),
        ("clang", "x86_64-linux-gnu", None),
        ("zig", "x86_64-linux-musl", None),
        ("clang", "x86_64-linux-android", "asm-x86")),
    4: (("clang", "i686-w64-mingw32", None),
        ("clang", "i686-pc-windows-msvc", None),
        ("clang", "i686-linux-gnu", None),
        ("zig", "x86-linux-musl", None),
        ("clang", "i686-linux-android", "asm-x86")),
}


def run(command, **options):
    return subprocess.run(command, capture_output=True, text=True, **options)


def diagnostics(result):
    """The compiler's diagnostics, or empty when it compiled cleanly."""
    text = (result.stdout + result.stderr).strip()
    if result.returncode == 0 and "warning" not in text:
        return ""
    return text or f"exit status {result.returncode}"


class Checker:
    def __init__(self, arguments, work):
        self.arguments = arguments
        self.work = work
        self.failures = 0
        self.skipped = []
        manifest = arguments.toolchains.expanduser() / "toolchains.json"
        self.toolchains = json.loads(manifest.read_text(encoding="utf-8")) \
            if manifest.exists() else {}

    def neverd(self, *options):
        command = [self.arguments.neverd, "types"]
        if self.arguments.system_library:
            command += ["--system-library", self.arguments.system_library]
        if self.arguments.declare:
            command += ["--data-model", self.arguments.data_model]
            if self.arguments.record_abi:
                command += ["--record-abi", self.arguments.record_abi]
            for path in self.arguments.declare:
                command += ["--declare", path]
        else:
            command.append(self.arguments.binary)
            if self.arguments.pdb:
                command += ["--pdb", self.arguments.pdb]
        result = run(command + list(options))
        if result.returncode:
            sys.exit(f"neverd types failed: {result.stderr.strip()}")
        return result.stdout

    def render(self, name, *options):
        path = self.work / name
        self.neverd("--all", "--asserts", "-o", str(path), *options)
        return path

    def report(self, what, problems):
        if problems:
            self.failures += 1
            lines = problems.splitlines()
            print(f"FAIL {what}")
            for line in lines[:20]:
                print(f"     {line}")
            if len(lines) > 20:
                print(f"     ... {len(lines) - 20} more lines")
        else:
            print(f"ok   {what}")

    def skip(self, what, why):
        self.skipped.append(what)
        print(f"skip {what}: {why}")

    def c_family(self, model):
        clang = shutil.which(self.arguments.clang)
        if not clang:
            self.skip("C and C++", f"{self.arguments.clang} not found")
            return
        msvc, gnu = TARGETS[model][:2]
        cxx = clang + "++" if shutil.which(clang + "++") else clang
        own = SYSTEMS.get(self.arguments.system_library)
        for spelling in SPELLINGS:
            for language, suffix in (("c", ".h"), ("cpp", ".hpp")):
                header = self.render(f"types-{spelling}{suffix}", "--lang",
                                     language, "--spelling", spelling)
                # The C header is C's and C++'s; the C++ one is C++'s.
                units = [("c++", "-std=c++20", header.stem + "-cpp.cpp")]
                if language == "c":
                    units.insert(0, ("c", "-std=c11", header.stem + ".c"))
                # Portable C is every compiler's; Windows' names are both
                # Windows compilers', MSVC's and MinGW's, whose long doubles
                # differ.
                targets = [("clang", msvc, None)] + (
                    [("clang", gnu, None)] if gnu and spelling == "portable"
                    else [])
                if spelling == "windows" and msvc in OTHER_WINDOWS_COMPILER:
                    targets.append(("clang", OTHER_WINDOWS_COMPILER[msvc],
                                    None))
                # A library of a system's headers takes its C library's
                # types from them on that system, and defines them itself
                # on every other, in its portable C.
                if own:
                    targets = [own] + ([other for other in OTHER_SYSTEMS[
                        8 if TARGETS[model][0].startswith(("x86_64",
                                                           "aarch64",
                                                           "arm64"))
                        else 4] if other[1] != own[1] and other[1] !=
                        SAME_SYSTEM.get(self.arguments.system_library)]
                        if spelling == "portable" else [])
                for unit_language, standard, name in units:
                    unit = self.work / name
                    unit.write_text(f'#include "{header.name}"\n',
                                    encoding="utf-8")
                    for tool, target, extra in targets:
                        command = self.compiler(tool, unit_language, target,
                                                extra, clang, cxx)
                        if command is None:
                            continue
                        result = run([*command, standard, "-x",
                                      unit_language, str(unit)], cwd=self.work)
                        what = language if unit_language == "c" or \
                            language == "cpp" else "c as c++"
                        self.report(f"{what} {spelling} {target}",
                                    diagnostics(result))

    def compiler(self, tool, language, target, extra, clang, cxx):
        """The command that compiles a unit of language for target alone:
        clang, with the toolchains' sysroot or bionic's headers extra names,
        or zig, whose own headers are musl's and macOS's; none, after
        reporting it skipped, where the toolchains lack it."""
        if tool == "zig":
            zig = self.toolchains.get("zig")
            if not zig or not Path(zig).exists():
                self.skip(f"C for {target}", FETCH)
                return None
            # zig cc compiles to an object: its -fsyntax-only does not run.
            cache = str(self.work / "zig-cache")
            return ["env", f"ZIG_GLOBAL_CACHE_DIR={cache}",
                    f"ZIG_LOCAL_CACHE_DIR={cache}", zig,
                    "cc" if language == "c" else "c++", f"--target={target}",
                    "-c", "-o", str(self.work / "zig.o")]
        command = [clang if language == "c" else cxx, f"--target={target}",
                   "-fsyntax-only"]
        # No C++ library's headers: the units include C's alone, which a
        # host without a 32-bit C++ library still has.
        if language != "c":
            command.append("-nostdinc++")
        if extra and extra.startswith("darwin:"):
            options = self.toolchains.get("sysroots", {}).get(
                "darwin", {}).get(extra.split(":", 1)[1])
            if not options:
                self.skip(f"C for {target}", FETCH)
                return None
            return [command[0], *options, *command[2:]]
        if extra in ("arm64", "armhf"):
            sysroot = self.toolchains.get("sysroots", {}).get(extra)
            if not sysroot:
                self.skip(f"C for {target}", FETCH)
                return None
            command.append(f"--sysroot={sysroot}")
        elif extra:
            bionic = self.toolchains.get("sysroots", {}).get("bionic")
            if not bionic:
                self.skip(f"C for {target}", FETCH)
                return None
            resources = run([clang, "-print-resource-dir"]).stdout.strip()
            command += ["-nostdinc", "-isystem", f"{resources}/include"]
            for directory in ("include", "kernel/uapi", f"kernel/uapi/{extra}",
                              "kernel/android/uapi"):
                command += ["-isystem", str(Path(bionic) / directory)]
            # The NDK's <jni.h> and <android/log.h>, which the Android
            # libraries declare too.
            sources = self.toolchains.get("sources", {})
            for key, directory in (("libnativehelper", "include_jni"),
                                   ("logging", "liblog/include")):
                if key in sources:
                    command += ["-isystem", str(Path(sources[key]) / directory)]
        return command

    def prototypes(self, model):
        """The prototypes, with Windows' typedefs, for the MSVC target, or
        for the system of --system-library, whose headers declare its
        routines in place of them."""
        clang = shutil.which(self.arguments.clang)
        if not clang:
            self.skip("prototypes", f"{self.arguments.clang} not found")
            return
        header = self.render("prototypes.h", "--lang", "c", "--spelling",
                             "windows", "--prototypes")
        unit = self.work / "prototypes.c"
        unit.write_text(WINDOWS_TYPEDEFS + f'#include "{header.name}"\n',
                        encoding="utf-8")
        tool, target, extra = SYSTEMS.get(self.arguments.system_library,
                                          ("clang", TARGETS[model][0], None))
        command = self.compiler(tool, "c", target, extra, clang, clang)
        if command is None:
            return
        result = run([*command, "-std=c11", "-x", "c", str(unit)],
                     cwd=self.work)
        self.report(f"c prototypes {target}", diagnostics(result))

    def rust(self, model):
        """rustc checks each size and offset as it compiles, for the
        program's target: the toolchains' rustc has the standard library of
        each, where the host's has its own alone."""
        target = TARGETS[model][4]
        rust = self.toolchains.get("rust", {})
        rustc, environment = rust.get("rustc"), None
        if rustc and Path(rustc).exists():
            environment = dict(os.environ, **rust.get("env", {}))
        elif target is None and shutil.which("rustc"):
            rustc = shutil.which("rustc")
        elif target is None:
            self.skip("Rust", "rustc not found")
            return
        else:
            self.skip("Rust", f"no rustc for {target}: {FETCH}")
            return
        for spelling in RUST_SPELLINGS:
            source = self.render(f"types-{spelling}.rs", "--lang", "rust",
                                 "--spelling", spelling)
            result = run([rustc, "--crate-type=lib", "--edition=2021",
                          "--emit=metadata", "-o",
                          str(self.work / f"types-{spelling}.rmeta")] +
                         ([f"--target={target}"] if target else []) +
                         [str(source)], env=environment)
            self.report(f"rust {spelling}" + (f" {target}" if target else ""),
                        diagnostics(result))

    def go(self, model):
        go = shutil.which("go")
        if not go:
            self.skip("Go", "go not found")
            return
        goos, goarch = TARGETS[model][2:4]
        environment = dict(os.environ, GOOS=goos, GOARCH=goarch,
                           GOFLAGS="-mod=mod")
        for spelling in GO_SPELLINGS:
            module = self.work / f"go-{spelling}"
            module.mkdir()
            self.render(f"go-{spelling}/types.go", "--lang", "go",
                        "--spelling", spelling)
            (module / "go.mod").write_text("module types\n\ngo 1.22\n",
                                           encoding="utf-8")
            result = run([go, "vet", "./..."], cwd=module, env=environment)
            self.report(f"go {spelling} {goarch}",
                        diagnostics(result))

    def python(self, model):
        """Python's ctypes check each size and offset as the module runs, on
        a Python of the program's kind: the host's, or the toolchains'
        i386 Linux one, 32-bit ARM's under qemu, 32-bit Windows' under
        Wine."""
        kind = TARGETS[model][5]
        environment, windows = None, False
        if kind == "host":
            python = shutil.which("python3")
            if not python:
                self.skip("Python", "python3 not found")
                return
            command = [python]
        else:
            runner = self.toolchains.get("python", {}).get(kind)
            if not runner or not all(Path(part).exists() for part in
                                     runner["command"]
                                     if part.startswith("/")):
                self.skip("Python", f"no {kind} Python: {FETCH}")
                return
            command = list(runner["command"])
            environment = {key: value for key, value in os.environ.items()
                           if key not in runner.get("unset", [])}
            environment.update(runner.get("env", {}))
            windows = runner.get("windows_paths", False)
        for spelling in PYTHON_SPELLINGS:
            source = self.render(f"types-{spelling}.py", "--lang", "python",
                                 "--spelling", spelling)
            # Wine reaches the host's files as drive Z:.
            path = f"Z:{source}" if windows else str(source)
            result = run(command + ["-I", "-W", "error", path],
                         env=environment)
            self.report(f"python {spelling}" +
                        ("" if kind == "host" else f" {kind}"),
                        diagnostics(result))

    def javascript(self):
        """Node runs the module, whose checks read and write every accessor
        within its record, and tsc checks its JSDoc as TypeScript does."""
        node = shutil.which("node")
        if not node:
            self.skip("JavaScript", "node not found")
            return
        tsc = shutil.which(self.arguments.tsc) or shutil.which(
            self.toolchains.get("tsc", ""))
        if not tsc:
            self.skip("TypeScript", f"{self.arguments.tsc} not found; {FETCH}")
        for spelling in JAVASCRIPT_SPELLINGS:
            # .mjs: Node reads the file as the module it is.
            module = self.render(f"types-{spelling}.mjs", "--lang",
                                 "javascript", "--spelling", spelling)
            self.report(f"javascript {spelling}",
                        diagnostics(run([node, str(module)])))
            if tsc:
                result = run([tsc, "--noEmit", "--allowJs", "--checkJs",
                              "--strict", "--target", "es2022", "--module",
                              "es2022", str(module)])
                self.report(f"typescript {spelling}", diagnostics(result))

    def lua(self):
        """Lua 5.3 or later runs the module, whose checks read and write
        every accessor within its record."""
        names = self.arguments.lua + [self.toolchains.get("lua", "")]
        lua = next(filter(None, (shutil.which(name) for name in names if name)),
                   None)
        if not lua:
            self.skip("Lua", f"none of {', '.join(self.arguments.lua)} found; "
                      f"{FETCH}")
            return
        for spelling in LUA_SPELLINGS:
            module = self.render(f"types-{spelling}.lua", "--lang", "lua",
                                 "--spelling", spelling)
            self.report(f"lua {spelling}",
                        diagnostics(run([lua, str(module)])))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", required=True,
                        help="the neverd command line tool")
    parser.add_argument("--binary", default=str(FIXTURES / "local_types.exe"),
                        help="the program whose types to check")
    parser.add_argument("--pdb", default=None,
                        help="its PDB, when the program does not find it")
    parser.add_argument("--declare", action="append", default=[],
                        help="check the types this file declares instead of a "
                             "program's; repeat for more")
    parser.add_argument("--data-model", default="LLP64",
                        choices=sorted(TARGETS),
                        help="the data model --declare files lay out for")
    parser.add_argument("--record-abi", default=None,
                        choices=("microsoft", "mingw-x64", "mingw-x86",
                                 "itanium", "apple-arm64", "i386-sysv",
                                 "android-i386", "aapcs32"),
                        help="the record ABI --declare files lay out by "
                             "(default: the data model's), as a 32-bit Linux "
                             "library's i386-sysv")
    parser.add_argument("--system-library", default=None,
                        choices=sorted(SYSTEMS),
                        help="the library of a system's headers whose types "
                             "the C and C++ take from that system's headers "
                             "on it (neverd types --system-library), checked "
                             "there and, in the portable C that defines "
                             "them, on every other system")
    parser.add_argument("--clang", default="clang",
                        help="the clang driver C and C++ compile with")
    parser.add_argument("--tsc", default="tsc",
                        help="the TypeScript compiler JavaScript's JSDoc "
                             "checks with")
    parser.add_argument("--lua", action="append", default=None,
                        help="a Lua 5.3+ interpreter to run Lua with; repeat "
                             "for others to try (default lua5.4, lua5.3, "
                             "lua)")
    parser.add_argument("--keep", default=None,
                        help="keep the rendered sources in this directory")
    parser.add_argument("--toolchains", type=Path,
                        default=Path.home() / ".cache" /
                        "neverd-render-toolchains",
                        help="the toolchains scripts/fetch_render_toolchains.py "
                             "fetched, which check 32-bit and cross layouts "
                             "(default ~/.cache/neverd-render-toolchains)")
    arguments = parser.parse_args()
    if arguments.lua is None:
        arguments.lua = ["lua5.4", "lua5.3", "lua"]
    if arguments.pdb is None and arguments.binary == str(
            FIXTURES / "local_types.exe"):
        arguments.pdb = str(FIXTURES / "local_types.pdb")

    with tempfile.TemporaryDirectory(prefix="neverd-types-") as scratch:
        work = Path(arguments.keep or scratch)
        work.mkdir(parents=True, exist_ok=True)
        checker = Checker(arguments, work)
        if arguments.declare:
            model = arguments.data_model
            if arguments.record_abi in TARGETS:
                model = arguments.record_abi
            names = checker.neverd().splitlines()
            types = sum(line.startswith("type ") for line in names)
            prototypes = sum(line.startswith("prototype ") for line in names)
            if not types:
                sys.exit("the declarations declare no types to check")
            checker.report(f"{types} types and {prototypes} prototypes read",
                           "")
        else:
            listing = json.loads(checker.neverd("--json"))
            model = listing.get("dataModel", "LLP64")
            if listing.get("recordABI") in TARGETS:
                model = listing["recordABI"]
            if listing.get("source") not in ("pdb", "dwarf") or \
                    not listing.get("types"):
                sys.exit(f"{arguments.binary} has no local types to check")
            stats = listing.get("stats", {})
            mismatches = stats.get("nestingMismatches", [])
            prototypes = 0
            checker.report(f"{len(listing['types'])} types read from "
                           f"{listing['source']}, nesting holds",
                           "\n".join(f"{name} does not lay out as listed"
                                     for name in mismatches))
            # What DWARF describes that local types leave out, with why.
            for entry in stats.get("skipped", []):
                print(f"left out {entry}")
        checker.c_family(model)
        if prototypes:
            checker.prototypes(model)
        checker.rust(model)
        checker.go(model)
        checker.python(model)
        checker.javascript()
        checker.lua()
    if checker.failures:
        sys.exit(f"{checker.failures} check(s) failed")
    print("every check passed" +
          (f"; skipped {', '.join(checker.skipped)}" if checker.skipped else ""))


if __name__ == "__main__":
    main()
