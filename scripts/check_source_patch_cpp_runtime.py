#!/usr/bin/env python3
"""Qualify source-produced x64/ARM64 FH3 with actual Windows processes."""

from __future__ import annotations

import argparse
import ctypes
import json
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from scripts import check_source_patch_pe_runtime as pe

SCHEMA = "neverd.source-patch.cpp-runtime.v1"
ARCHES = ("x64", "arm64")
ORIGINAL_CASES = ((0, 1), (8, 77), (9, 19))
PATCHED_CASES = ((0, 40), (1, 10), (2, 21), (3, 27), (4, 31), (5, 77),
                 (6, 40), (7, 33), (9, 19))
ORIGINAL = r'''
extern "C" __declspec(dllimport) void _exit(int);
using terminate_handler = void (*)();
extern "C" __declspec(dllimport) terminate_handler set_terminate(terminate_handler);
extern "C" __declspec(dllexport) int cleanup_count = 0;
struct Guard { ~Guard() { ++cleanup_count; } };
extern "C" __declspec(dllexport) __declspec(noinline) int original_thrower(int x) {
  if (x) throw x; return 0;
}
extern "C" __declspec(dllexport) __declspec(noinline) int target(int x) {
  volatile int value = x; if (value == -999) throw value; return value + 1;
}
__declspec(noinline) int reraiser(int x) {
  try { return original_thrower(x); } catch (...) { Guard g; throw; }
}
extern "C" __declspec(dllexport) __declspec(noinline) int original_noexcept(int x) noexcept {
  return original_thrower(x);
}
void on_terminate() { _exit(77); }
int main(int argc, char **argv) {
  set_terminate(on_terminate);
  int x = argc > 1 ? argv[1][0] - '0' : 0;
  if (x == 8) return original_noexcept(x);
  try { if (x == 9) return reraiser(9); return target(x); }
  catch (int value) { return value + cleanup_count * 10; }
}
'''
REPLACEMENT = r'''
extern "C" int cleanup_count;
extern "C" int original_thrower(int);
struct Guard { ~Guard() { ++cleanup_count; } };
__declspec(noinline) int broken() noexcept { throw 31; }
extern "C" int replacement(int x) {
  if (x == 5) return broken();
  if (x == 0) return 40;
  if (x == 1) { try { throw 7; } catch (int value) { return value + 3; } }
  if (x == 2) { Guard g; throw 11; }
  if (x == 3) { Guard g; return original_thrower(17); }
  if (x == 6) {
    int first = 0;
    try { Guard g; try { throw 13; } catch (int value) { first = value; throw 17; } }
    catch (int value) { return first + value + cleanup_count * 10; }
  }
  if (x == 7) {
    Guard outer;
    try { Guard inner; return original_thrower(23); }
    catch (int value) { return value + cleanup_count * 10; }
  }
  try { throw 21; } catch (...) { Guard g; throw; }
}
'''


def exports(library: Path, image: Path) -> dict[str, int]:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pluginsdk/python"))
    from neverd_plugin.ffi import HostAPI
    host = HostAPI(ctypes.CDLL(str(library)))
    session = host.function("neverd_session_create")()
    if not session:
        raise RuntimeError("session creation failed")
    try:
        if host.function("neverd_session_load")(session, os.fsencode(image)) != 1:
            raise RuntimeError(host.owned_string("neverd_last_error", session))
        rows = json.loads(host.owned_string("neverd_exports_json", session) or "[]")
        result = {}
        for name in ("target", "cleanup_count", "??_7type_info@@6B@"):
            matches = [row for row in rows if row["name"] == name]
            if len(matches) != 1:
                raise ValueError(f"missing/ambiguous native export {name}")
            result[name] = int(matches[0]["addr"], 0)
        return result
    finally:
        host.function("neverd_session_destroy")(session)


def prepare(args: argparse.Namespace) -> None:
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    manifest = {"schema": SCHEMA, "commit": os.environ.get("GITHUB_SHA"),
                "neverd_sha256": pe.digest(args.neverd), "images": []}
    for arch in ARCHES:
        directory = root / arch
        directory.mkdir()
        source, obj, original, patched = (directory / name for name in
            ("original.cpp", "original.obj", "original.exe", "patched.exe"))
        source.write_text(ORIGINAL, encoding="utf-8")
        pe.run([str(args.clang), "--driver-mode=g++", "--target=" + pe.PROFILES[arch][0],
                "-O0", "-fms-runtime-lib=dll", "-c", str(source), "-o", str(obj)],
               directory / "original-compile.log")
        crt, ucrt, sdk = (getattr(args, name + "_" + arch) for name in ("crt", "ucrt", "sdk"))
        pe.run([str(args.linker), str(obj), "/out:" + str(original), "/subsystem:console",
                "/export:??_7type_info@@6B@,DATA", "/dynamicbase",
                "/libpath:" + str(crt), "/libpath:" + str(ucrt), "/libpath:" + str(sdk)],
               directory / "original-link.log")
        addresses = exports(args.library, original)
        original_hash = pe.digest(original)
        replacement = directory / "replacement.cpp"
        replacement.write_text(REPLACEMENT, encoding="utf-8")
        command = [str(args.neverd), "patch", str(original), "--from-source=" + str(replacement),
                   "--compiler-path=" + str(args.clang), "--language=c++", "--compiler-arg=-O0",
                   "--entry=replacement", f"--func={addresses['target']:#x}", "--patch-report",
                   f"--bind=data:cleanup_count={addresses['cleanup_count']:#x}:4",
                   f"--bind=const:??_7type_info@@6B@={addresses['??_7type_info@@6B@']:#x}:8",
                   "-o", str(patched)]
        result = pe.run(command, directory / "patch.log")
        receipt = pe.publication_receipt(result.stdout, arch, "ir")
        (directory / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        if pe.digest(original) != original_hash:
            raise ValueError("source C++ installation altered its original")
        for kind, image in (("original", original), ("patched", patched)):
            if pe.machine(image) != pe.PROFILES[arch][1]:
                raise ValueError("source C++ PE machine mismatch")
            manifest["images"].append({"architecture": arch, "kind": kind,
                "path": image.relative_to(root).as_posix(), "sha256": pe.digest(image)})
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def execute(args: argparse.Namespace) -> None:
    root = args.root.resolve()
    output = root / (args.arch + "-runtime.json")
    output.unlink(missing_ok=True)
    host = pe.runtime_host(args.arch, args.runner, args.expected_native_host)
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("schema") != SCHEMA:
        raise ValueError("unknown C++ runtime schema")
    if os.environ.get("GITHUB_SHA") and manifest.get("commit") != os.environ["GITHUB_SHA"]:
        raise ValueError("C++ fixtures belong to a different commit")
    rows = [row for row in manifest["images"] if row["architecture"] == args.arch]
    if len(rows) != 2 or {row["kind"] for row in rows} != {"original", "patched"}:
        raise ValueError("missing/duplicate C++ runtime image inventory")
    observations = []
    for row in rows:
        image = (root / row["path"]).resolve()
        if not image.is_relative_to(root) or pe.digest(image) != row["sha256"]:
            raise ValueError("C++ runtime image path/digest mismatch")
        if pe.machine(image) != pe.PROFILES[args.arch][1]:
            raise ValueError("C++ runtime image machine mismatch")
        cases = ORIGINAL_CASES if row["kind"] == "original" else PATCHED_CASES
        for argument, expected in cases:
            for attempt in range(3):
                command = ([str(args.runner)] if args.runner else []) + [str(image), str(argument)]
                result = pe.run(command, image.with_suffix(f".mode-{argument}-run-{attempt}.log"), expected)
                observations.append({"kind": row["kind"], "path": row["path"], "sha256": row["sha256"],
                    "mode": argument, "attempt": attempt, "expected": expected, "actual": result.returncode})
    output.write_text(json.dumps({"schema": SCHEMA, "commit": manifest["commit"],
        "architecture": args.arch, "native_host": host, "observations": observations}, indent=2) + "\n", encoding="utf-8")
    print(f"{args.arch}: all 36 C++ Windows process observations passed ({host})")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prep = commands.add_parser("prepare")
    for name in ("neverd", "library", "clang", "linker", "output"):
        prep.add_argument("--" + name, type=Path, required=True)
    for arch in ARCHES:
        for name in ("crt", "ucrt", "sdk"):
            prep.add_argument(f"--{name}-{arch}", type=Path, required=True)
    check = commands.add_parser("run")
    check.add_argument("--root", type=Path, required=True)
    check.add_argument("--arch", choices=ARCHES, required=True)
    check.add_argument("--expected-native-host", choices=ARCHES)
    check.add_argument("--runner", type=Path)
    args = parser.parse_args()
    if args.command == "prepare":
        for name in ("neverd", "library", "clang", "linker"):
            path = getattr(args, name).absolute()
            if not path.is_file():
                parser.error(f"missing {name}: {path}")
            setattr(args, name, path)
        prepare(args)
    else:
        execute(args)


if __name__ == "__main__":
    main()
