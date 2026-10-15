#!/usr/bin/env python3
"""Compile a program's local types as NeverD renders them.

`neverd types --all --asserts` renders every local type with compile-time
checks of each size and offset.  This script renders them in C and C++ with
every builtin spelling, in Rust and in Go, and compiles each with clang,
rustc and go, so that a layout NeverD gets wrong, or source that does not
compile, fails.  Without --binary it checks the fixture PDB the unit tests
use.  A toolchain that is missing is reported and skipped.
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
SPELLINGS = ("windows", "ida", "keywords", "portable")

# Per data model: the MSVC target C and C++ compile for, the GNU target the
# portable spelling also compiles for, and Go's GOARCH.
TARGETS = {
    "LLP64": ("x86_64-pc-windows-msvc", "x86_64-linux-gnu", "amd64"),
    "ILP32": ("i686-pc-windows-msvc", "i686-linux-gnu", "386"),
    "LP64": ("x86_64-linux-gnu", "x86_64-linux-gnu", "amd64"),
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

    def neverd(self, *options):
        command = [self.arguments.neverd, "types", self.arguments.binary]
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
        msvc, gnu, _ = TARGETS[model]
        for spelling in SPELLINGS:
            for language, suffix, standard in (("c", ".h", "-std=c11"),
                                               ("cpp", ".hpp", "-std=c++20")):
                header = self.render(f"types-{spelling}{suffix}", "--lang",
                                     language, "--spelling", spelling)
                unit = header.with_suffix(".c" if language == "c" else ".cpp")
                unit.write_text(f'#include "{header.name}"\n', encoding="utf-8")
                targets = [msvc] + ([gnu] if spelling == "portable" else [])
                for target in targets:
                    driver = clang if language == "c" else clang + "++"
                    if language == "cpp" and not shutil.which(driver):
                        driver = clang
                    result = run([driver, f"--target={target}", "-fsyntax-only",
                                  standard, "-x",
                                  "c" if language == "c" else "c++", str(unit)],
                                 cwd=self.work)
                    self.report(f"{language} {spelling} {target}",
                                diagnostics(result))

    def rust(self, model):
        rustc = shutil.which("rustc")
        if not rustc:
            self.skip("Rust", "rustc not found")
            return
        if model == "ILP32":
            self.skip("Rust", "32-bit layouts need a 32-bit Rust target")
            return
        source = self.render("types.rs", "--lang", "rust")
        result = run([rustc, "--crate-type=lib", "--edition=2021",
                      "--emit=metadata", "-o", str(self.work / "types.rmeta"),
                      str(source)])
        self.report("rust", diagnostics(result))

    def go(self, model):
        go = shutil.which("go")
        if not go:
            self.skip("Go", "go not found")
            return
        module = self.work / "go"
        module.mkdir()
        source = self.render("go/types.go", "--lang", "go")
        (module / "go.mod").write_text("module types\n\ngo 1.22\n",
                                       encoding="utf-8")
        environment = dict(os.environ, GOOS="windows",
                           GOARCH=TARGETS[model][2], GOFLAGS="-mod=mod")
        result = run([go, "vet", "./..."], cwd=module, env=environment)
        self.report(f"go {TARGETS[model][2]}", diagnostics(result))
        del source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", required=True,
                        help="the neverd command line tool")
    parser.add_argument("--binary", default=str(FIXTURES / "local_types.exe"),
                        help="the program whose types to check")
    parser.add_argument("--pdb", default=None,
                        help="its PDB, when the program does not find it")
    parser.add_argument("--clang", default="clang",
                        help="the clang driver C and C++ compile with")
    parser.add_argument("--keep", default=None,
                        help="keep the rendered sources in this directory")
    arguments = parser.parse_args()
    if arguments.pdb is None and arguments.binary == str(
            FIXTURES / "local_types.exe"):
        arguments.pdb = str(FIXTURES / "local_types.pdb")

    with tempfile.TemporaryDirectory(prefix="neverd-types-") as scratch:
        work = Path(arguments.keep or scratch)
        work.mkdir(parents=True, exist_ok=True)
        checker = Checker(arguments, work)
        listing = json.loads(checker.neverd("--json"))
        model = listing.get("dataModel", "LLP64")
        if listing.get("source") != "pdb" or not listing.get("types"):
            sys.exit(f"{arguments.binary} has no local types to check")
        mismatches = listing.get("stats", {}).get("nestingMismatches", [])
        checker.report(f"{len(listing['types'])} types read, nesting holds",
                       "\n".join(f"{name} does not lay out as listed"
                                 for name in mismatches))
        checker.c_family(model)
        checker.rust(model)
        checker.go(model)
    if checker.failures:
        sys.exit(f"{checker.failures} check(s) failed")
    print("every check passed" +
          (f"; skipped {', '.join(checker.skipped)}" if checker.skipped else ""))


if __name__ == "__main__":
    main()
