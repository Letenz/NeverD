#!/usr/bin/env python3
"""Prepare source/object PE patches and require real Windows process execution."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys


PROFILES = {
    "x86": ("i686-pc-windows-msvc", 0x14C),
    "x64": ("x86_64-pc-windows-msvc", 0x8664),
    "arm64": ("aarch64-pc-windows-msvc", 0xAA64),
}
ARTIFACTS = ("ir", "object", "supplied-object")
ORIGINAL = r"""
int _fltused = 0;
__declspec(dllexport) __declspec(noinline) int original_helper(int x) {
  volatile int y = x;
  return y + 1;
}
__declspec(dllexport) __declspec(noinline) double original_fp(
    float a, double b, int x, int c, int d, int e, int f, int g, int h, int i) {
  return a * 2 + b + x + c + d + e + f + g + h + i;
}
__declspec(dllexport) __declspec(noinline) int target(int x) {
  return original_helper(x);
}
int entry(void) {
  if (original_helper(4) != 5) return 250;
  int a = target(2);
  int b = target(5);
  if (original_helper(9) != 10) return 251;
  return a + b;
}
"""
CASES = {
    "nonleaf": (87, r"""
extern int original_helper(int);
int replacement(int x) {
  volatile int y = original_helper(x);
  return y + 39;
}
"""),
    "writable-pointer": (92, r"""
extern int original_helper(int);
static volatile int counter = 40;
static volatile int *volatile pointer = &counter;
int replacement(int x) {
  ++*pointer;
  return *pointer + original_helper(x);
}
"""),
    "floating-stack": (93, r"""
extern double original_fp(float, double, int, int, int, int, int, int, int, int);
int replacement(int x) {
  return (int)original_fp(2.5f, 3.5, x, 2, 3, 4, 5, 6, 7, 8);
}
"""),
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def machine(path: Path) -> int:
    data = path.read_bytes()
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError(f"not a PE fixture: {path}")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe > len(data) - 6 or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"missing PE header: {path}")
    return struct.unpack_from("<H", data, pe + 4)[0]


def run(command: list[str], log: Path, expected: int = 0) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=60)
    log.write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode != expected:
        raise RuntimeError(f"{command[0]} returned {result.returncode}, "
                           f"expected {expected}; see {log}")
    return result


def publication_receipt(stdout: str, arch: str, artifact: str) -> dict:
    reports = [json.loads(line) for line in stdout.splitlines()
               if line.startswith('{')]
    reports = [report for report in reports
               if report.get("schema") == "neverd.source-patch.v1"]
    if len(reports) != 1 or reports[0].get("success") is not True:
        raise ValueError("missing successful publication receipt")
    report = reports[0]
    target = report.get("target")
    triple = PROFILES[arch][0]
    # Clang IR may append the MSVC version. Objects report the loaded image's
    # canonical ABI, independently of the object producer's host environment.
    valid = (isinstance(target, str) and
             (re.fullmatch(re.escape(triple) + r"(?:\d+(?:\.\d+)*)?", target)
              if artifact == "ir" else target == triple))
    if not valid:
        raise ValueError(f"publication receipt target mismatch for {arch}/{artifact}")
    return report


def function_addresses(library: Path, image: Path) -> dict[str, int]:
    # Reuse the public ABI's export inventory, not a second
    # PE export parser or fixed addresses from a particular linker layout.
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pluginsdk/python"))
    from neverd_plugin.ffi import HostAPI

    host = HostAPI(ctypes.CDLL(str(library)))
    session = host.function("neverd_session_create")()
    if not session:
        raise RuntimeError("session creation failed")
    try:
        if host.function("neverd_session_load")(session, os.fsencode(image)) != 1:
            raise RuntimeError(host.owned_string("neverd_last_error", session))
        exports = json.loads(host.owned_string("neverd_exports_json", session) or "[]")
        addresses = {}
        for name in ("target", "original_helper", "original_fp"):
            matches = [row for row in exports if row["name"] in (name, "_" + name)]
            if len(matches) != 1:
                raise RuntimeError(f"missing or ambiguous exported function {name}")
            addresses[name] = int(matches[0]["addr"], 0)
        return addresses
    finally:
        host.function("neverd_session_destroy")(session)


def prepare(args: argparse.Namespace) -> None:
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    manifest = {"schema": "neverd.source-patch.pe-runtime.v1",
                "commit": os.environ.get("GITHUB_SHA"),
                "neverd_sha256": digest(args.neverd), "images": []}
    for arch, (triple, expected_machine) in PROFILES.items():
        directory = root / arch
        directory.mkdir()
        source, obj, original = (directory / n for n in
                                 ("original.c", "original.obj", "original.exe"))
        source.write_text(ORIGINAL, encoding="utf-8")
        run([str(args.clang), "--target=" + triple, "-O0", "-c", str(source),
             "-o", str(obj)], directory / "original-compile.log")
        # PE32 has no mandatory runtime-function table. Retain typed COFF
        # symbols to authenticate non-entry functions; exports alone are untyped.
        run([str(args.linker), str(obj), "/out:" + str(original),
             "/entry:entry", "/subsystem:console", "/nodefaultlib",
             "/dynamicbase", "/debug:dwarf"], directory / "original-link.log")
        addresses = function_addresses(args.library, original)
        original_hash = digest(original)

        def record(path: Path, case: str, artifact: str, expected: int) -> None:
            if machine(path) != expected_machine:
                raise RuntimeError(f"wrong PE machine for {arch}: {path}")
            manifest["images"].append({"architecture": arch, "case": case,
                                       "artifact": artifact, "expected": expected,
                                       "path": path.relative_to(root).as_posix(),
                                       "sha256": digest(path)})

        record(original, "original", "original", 9)
        for case, (expected, text) in CASES.items():
            source = directory / (case + ".c")
            source.write_text(text, encoding="utf-8")
            for artifact in ARTIFACTS:
                output = directory / (case + "-" + artifact + ".exe")
                command = [str(args.neverd), "patch", str(original),
                           "--entry=replacement", f"--func={addresses['target']:#x}",
                           "--patch-report", "-o", str(output)]
                binding = "original_fp" if case == "floating-stack" else "original_helper"
                command.append(f"--bind=function:{binding}={addresses[binding]:#x}")
                if artifact == "supplied-object":
                    supplied = directory / (case + ".obj")
                    run([str(args.clang), "--target=" + triple, "-O2", "-fno-lto",
                         "-funwind-tables", "-ffunction-sections", "-c", str(source),
                         "-o", str(supplied)], directory / (case + "-compile.log"))
                    command.append("--from-object=" + str(supplied))
                else:
                    command.extend(["--from-source=" + str(source),
                                    "--compiler-path=" + str(args.clang),
                                    "--artifact=" + artifact])
                result = run(command, output.with_suffix(".log"))
                receipt = publication_receipt(result.stdout, arch, artifact)
                output.with_suffix(".receipt.json").write_text(
                    json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
                record(output, case, artifact, expected)
                if digest(original) != original_hash:
                    raise RuntimeError("patching modified the original image")
    if len(manifest["images"]) != 30:
        raise RuntimeError("incomplete PE runtime fixture inventory")
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                        encoding="utf-8")
    print("Prepared 3 originals and 27 published PE replacements")


def native_machine() -> int:
    if os.name != "nt":
        raise RuntimeError("native Windows execution is required")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    query = kernel.IsWow64Process2
    query.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ushort),
                     ctypes.POINTER(ctypes.c_ushort)]
    query.restype = ctypes.c_int
    process, native = ctypes.c_ushort(), ctypes.c_ushort()
    if not query(kernel.GetCurrentProcess(), ctypes.byref(process), ctypes.byref(native)):
        raise ctypes.WinError(ctypes.get_last_error())
    return native.value


def execute(args: argparse.Namespace) -> None:
    root = args.root.resolve()
    report_path = root / (args.arch + "-runtime.json")
    # A failed replay must not leave an earlier success for artifact consumers.
    report_path.unlink(missing_ok=True)
    if args.runner:
        if args.expected_native_host:
            raise ValueError("an external runner cannot claim native Windows coverage")
        host = "external-runner"
    else:
        if args.expected_native_host is None:
            raise ValueError("--expected-native-host is required for native qualification")
        actual = native_machine()
        expected = PROFILES[args.expected_native_host][1]
        if actual != expected:
            raise RuntimeError(f"native machine {actual:#x} differs from {expected:#x}")
        host = args.expected_native_host
        if args.arch == "arm64" and host != "arm64":
            raise ValueError("ARM64 fixtures require native Windows ARM64")
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("schema") != "neverd.source-patch.pe-runtime.v1":
        raise ValueError("unknown runtime manifest schema")
    if os.environ.get("GITHUB_SHA") and manifest.get("commit") != os.environ["GITHUB_SHA"]:
        raise ValueError("runtime fixtures belong to a different commit")
    images = [row for row in manifest["images"] if row["architecture"] == args.arch]
    inventory = {(row["case"], row["artifact"]) for row in images}
    wanted = {("original", "original")} | {(case, artifact) for case in CASES
                                            for artifact in ARTIFACTS}
    if len(images) != 10 or inventory != wanted:
        raise ValueError("missing or duplicate PE runtime cases")
    observations = []
    for row in images:
        image = (root / row["path"]).resolve()
        if not image.is_relative_to(root) or digest(image) != row["sha256"]:
            raise ValueError("runtime fixture path or digest mismatch")
        if machine(image) != PROFILES[args.arch][1]:
            raise ValueError("runtime fixture machine mismatch")
        # Expected observations are independent of the downloaded manifest.
        expected = 9 if row["case"] == "original" else CASES[row["case"]][0]
        if row["expected"] != expected:
            raise ValueError("runtime fixture expected result mismatch")
        for attempt in range(3):
            command = ([str(args.runner)] if args.runner else []) + [str(image)]
            result = run(command, image.with_suffix(f".run-{attempt}.log"), expected)
            observations.append({"path": row["path"], "sha256": row["sha256"],
                                 "attempt": attempt, "expected": expected,
                                 "actual": result.returncode})
    result = {"schema": manifest["schema"], "commit": manifest["commit"],
              "architecture": args.arch, "native_host": host,
              "observations": observations}
    report_path.write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{args.arch}: all 30 Windows process observations passed ({host})")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prep = commands.add_parser("prepare")
    for name in ("neverd", "library", "clang", "linker", "output"):
        prep.add_argument("--" + name, type=Path, required=True)
    check = commands.add_parser("run")
    check.add_argument("--root", type=Path, required=True)
    check.add_argument("--arch", choices=PROFILES, required=True)
    check.add_argument("--expected-native-host", choices=("x64", "arm64"))
    check.add_argument("--runner", type=Path)
    args = parser.parse_args()
    if args.command == "prepare":
        for name in ("neverd", "library", "clang", "linker"):
            path = getattr(args, name).absolute()
            if not path.is_file():
                parser.error(f"missing {name}: {path}")
            # Preserve argv[0]: lld-link and clang++ use the symlink's name
            # to select their driver, independently of the executable inode.
            setattr(args, name, path)
        prepare(args)
    else:
        execute(args)


if __name__ == "__main__":
    main()
