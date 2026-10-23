#!/usr/bin/env python3
"""Check public UCRT calls against the exact declarations used to build them.

Requires an --architecture all --ucrt --ucrt-compatibility generation directory
with its retained per-target inputs, not just the six published headers.
Compilation checks call types and arity; it does not certify native execution.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time


FIXTURES = Path(__file__).resolve().parents[1] / "unittests/types/fixtures"
FUNCTIONS = {
    "UseCRTMath": "_o__hypot", "UseCRTMathFloat": "_o__hypotf",
    "UseCRTComplex": "cpow", "UseCRTComplexMagnitude": "cabs",
    "UseCRTUnicode": "_o__wcsicmp", "UseCRTVarargs": "_wopen",
    "UseCRTThread": "_o__beginthreadex", "UseCRTTerminator": "_o_set_terminate",
    "UseCRTExceptionCopy": "_o___std_exception_copy",
    "UseCRTTypeName": "_o___std_type_info_name", "UseCRTException": "_o__except1",
    "UseCRTTypeCleanup": "_o___std_type_info_destroy_list",
}


def run(command, log, timeout):
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    log.write_text(result.stderr, encoding="utf-8")
    return result


def check(args):
    args.output.mkdir(parents=True, exist_ok=True)
    records, products = [], []
    for arch in args.architecture or ("x64", "x86", "arm64"):
        inputs = args.generated / arch
        manifest = json.loads((inputs / "manifest.json").read_text())
        provider = next(p for library in manifest["libraries"]
                        for p in library["providers"] if p["name"] == "UCRT")
        preprocess = provider["commands"][0]
        if preprocess[-2] != "-E":
            raise ValueError("UCRT provider has no preprocessing command")
        compiler = preprocess[:-2]
        output = args.output / arch
        output.mkdir(exist_ok=True)
        objects, commands = [], []
        for stem in ("windows_ucrt_calls.c", "windows_ucrt_callbacks.cpp",
                     "windows_ucrt_compatibility.c"):
            obj = output / (Path(stem).stem + ".obj")
            command = compiler + ["-O1", "-fno-stack-protector", "-fno-exceptions",
                                  "-c", str(FIXTURES / stem), "-o", str(obj)]
            run(command, output / (stem + ".log"), args.timeout).check_returncode()
            objects.append(str(obj))
            commands.append(command)
        library = inputs / "UCRT-inputs/kit/c/ucrt" / arch / "ucrt.osmode_permissive.lib"
        binary = output / "calls.dll"
        command = [args.linker, "/dll", "/noentry", "/nodefaultlib", "/timestamp:0",
                   "/out:" + str(binary), *objects, str(library)]
        run(command, output / "link.log", args.timeout).check_returncode()
        commands.append(command)
        products.append({"architecture": arch, "commands": commands,
                         "sha256": hashlib.sha256(binary.read_bytes()).hexdigest()})
        for function, api in FUNCTIONS.items():
            if args.function and function not in args.function:
                continue
            for route in ("highc", "llvmc"):
                stem = output / (function + "-" + route)
                command = [args.neverd, "decompile", str(binary), "--func", function]
                if route == "llvmc":
                    command.append("--llvm")
                start = time.monotonic()
                result = run(command, stem.with_suffix(".log"), args.timeout)
                elapsed = time.monotonic() - start
                source = "\n".join(line for line in result.stdout.splitlines()
                                   if not line.startswith(("=== NeverD", "File:", "Arch:")))
                path = stem.with_suffix(".c")
                path.write_text(source + "\n", encoding="utf-8")
                # An invented old-style extern can compile while passing no
                # arguments. Force the original declarations into the check.
                compile_command = compiler + [
                    "-include", str(inputs / "UCRT-inputs/UCRT.c"),
                    "-include", str(inputs / "UCRT-inputs/UCRTCompatibility.c"),
                    "-fsyntax-only", str(path)]
                compiled = run(compile_command, stem.with_suffix(".clang.log"), args.timeout)
                success = (result.returncode == 0 and compiled.returncode == 0 and
                           api + "(" in source and "__builtin_trap" not in source and
                           "unknown value" not in source)
                row = {"architecture": arch, "function": function, "api": api,
                       "route": route, "success": success, "seconds": elapsed,
                       "exit": result.returncode, "clang_exit": compiled.returncode,
                       "commands": [command, compile_command]}
                records.append(row)
                print(f"{arch} {function} {route}: {'PASS' if success else 'FAIL'}", flush=True)
    (args.output / "results.json").write_text(
        json.dumps({"products": products, "checks": records}, indent=2) + "\n")
    return all(record["success"] for record in records)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated", type=lambda p: Path(p).resolve(), required=True)
    parser.add_argument("--output", type=lambda p: Path(p).resolve(), required=True)
    parser.add_argument("--neverd", default="build-release/bin/neverd")
    parser.add_argument("--linker", default="lld-link")
    parser.add_argument("--architecture", action="append", choices=("x64", "x86", "arm64"))
    parser.add_argument("--function", action="append", choices=tuple(FUNCTIONS))
    parser.add_argument("--timeout", type=float, default=60)
    return 0 if check(parser.parse_args()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
