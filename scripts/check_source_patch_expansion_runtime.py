#!/usr/bin/env python3
"""Execute callback/layout/dependency patches through the native OS loader.

The CPU matrix is separate. This program never supplies import addresses or
executes guest instructions on behalf of the operating system.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import platform
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "pluginsdk/python"))
from neverd_plugin.api import Session
from neverd_plugin.ffi import HostAPI

SCHEMA = "neverd.source-patch.expansion.native.v3"
ARGUMENTS = (-5, 0, 7, 25)
VARIANTS = (
    "callback-ir",
    "callback-object",
    "dependency-ir",
    "dependency-object",
    "combined-ir",
    "combined-object",
)
ORIGINAL = r"""
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#define IMPORT __declspec(dllimport)
#else
#define EXPORT __attribute__((visibility("default")))
#define IMPORT
#endif
IMPORT int old_add(int);
IMPORT extern int old_data;
EXPORT int original_state = 0;
EXPORT __attribute__((noinline)) int target(int x) {
  volatile int y = x; return y + 1;
}
EXPORT __attribute__((noinline)) int second(int x) {
  volatile int y = x; return y + 7;
}
EXPORT __attribute__((noinline)) double target_fp(float a, double b,
    int x, int c, int d, int e, int f, int g, int h, int i) {
  return a*2+b+x+c+d+e+f+g+h+i;
}
EXPORT __attribute__((noinline)) int stateful(int x) {
  ++original_state; return x + original_state;
}
int main(int argc, char **argv) {
  if (argc != 2) return 99;
  int x = atoi(argv[1]);
  int a = target(x), b = second(x);
  int c = (int)target_fp(2.5f, 3.5, x, 2, 3, 4, 5, 6, 7, 8);
  int d = stateful(x), e = stateful(x), f = old_add(x) + old_data;
  printf("%d %d %d %d %d %d %p\n", a,b,c,d,e,f,(void *)&target);
  return 0;
}
"""
CALLBACKS = r"""
extern int saved(int), saved_second(int), saved_state(int);
extern double saved_fp(float,double,int,int,int,int,int,int,int,int);
int replacement(int x) { return saved(x)+saved(x+1)+40; }
int replace_second(int x) { return saved_second(x)*2; }
double replace_fp(float a,double b,int x,int c,int d,int e,int f,int g,int h,int i) {
  return saved_fp(a,b,x,c,d,e,f,g,h,i)+10;
}
int replace_state(int x) { return saved_state(x)+20; }
"""
DEPENDENCY = r"""
extern int dependency_fn(int);
extern int dependency_data;
int replacement(int x) { return dependency_fn(x)+dependency_data; }
"""
COMBINED = CALLBACKS.replace(
    "saved(x)+saved(x+1)+40", "dependency_fn(x)+dependency_data+saved(x)"
)
COMBINED = "extern int dependency_fn(int); extern int dependency_data;\n" + COMBINED


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def native_architecture(machine: str) -> str | None:
    return {
        "amd64": "x64",
        "x86_64": "x64",
        "arm64": "arm64",
        "aarch64": "arm64",
    }.get(machine.lower())


def write_patch_source(path: Path, code: str) -> None:
    # The receipt hashes the UTF-8 bytes passed to the API, without OS newline
    # translation or a platform-specific text encoding.
    path.write_bytes(code.encode("utf-8"))


def process_image_name(command: str, system: str) -> str:
    path = PureWindowsPath if system == "Windows" else PurePosixPath
    return path(command).name


def expected(variant: str, x: int) -> list[int]:
    values = [x + 1, x + 7, x + 43, x + 1, x + 2, x + 20]
    if variant.startswith(("callback-", "combined-")):
        values[:5] = [2 * x + 43, 2 * x + 14, x + 53, x + 21, x + 22]
    if variant.startswith("dependency-"):
        values[0] = 5 * x + 39
    if variant.startswith("combined-"):
        values[0] = 6 * x + 40
    return values


def run(
    command: list[str], log: Path, *, allow_failure=False
) -> subprocess.CompletedProcess:
    result = subprocess.run(command, text=True, capture_output=True, timeout=90)
    log.write_text(
        json.dumps(
            {
                "command": command,
                "returncode": result.returncode,
                "stdout": result.stdout,
                "stderr": result.stderr,
            },
            indent=2,
        )
        + "\n"
    )
    if not allow_failure and result.returncode:
        raise RuntimeError(
            f"{log.name}: exit {result.returncode}: {result.stderr[-3000:]}"
        )
    return result


def audit(root: Path) -> dict:
    report = json.loads((root / "report.json").read_text())
    if report["schema"] != SCHEMA or report["execution"] != "native-os-process":
        raise ValueError("not a native expansion report")
    if set(report["images"]) != {"original", *VARIANTS}:
        raise ValueError("incomplete image inventory")
    for row in [*report["images"].values(), *report["providers"].values()]:
        path = (root / row["path"]).resolve()
        if not path.is_relative_to(root.resolve()) or digest(path) != row["sha256"]:
            raise ValueError("image/provider digest mismatch")
    for name, sha in report["evidence_files"].items():
        path = (root / name).resolve()
        if not path.is_relative_to(root.resolve()) or digest(path) != sha:
            raise ValueError("evidence digest mismatch")
    for variant in VARIANTS:
        receipt = json.loads((root / (variant + "-receipt.json")).read_text())
        source = root / (variant + "-source.c")
        callback = variant.startswith(("callback-", "combined-"))
        deps = not variant.startswith("callback-")
        if (
            receipt.get("success") is not True
            or receipt.get("schema") != "neverd.source-patch.v1"
            or receipt["source_sha256"] != digest(source)
            or len(receipt["installed"]) != (4 if callback else 1)
            or len(receipt["original_callbacks"]) != (4 if callback else 0)
            or len(receipt["added_imports"]) != (2 if deps else 0)
        ):
            raise ValueError("publication receipt/source inventory mismatch")
    if set(report["providers"]) != {"old", "new"}:
        raise ValueError("incomplete provider inventory")
    expected_files = {
        p.relative_to(root).as_posix()
        for p in root.rglob("*")
        if p.is_file() and p != root / "report.json"
    }
    if set(report["evidence_files"]) != expected_files:
        raise ValueError("incomplete evidence file inventory")
    for variant in VARIANTS:
        receipt = json.loads((root / (variant + "-receipt.json")).read_text())
        validate_receipt(receipt, report["system"], report["architecture"], variant)
    keys = set()
    for row in report["observations"]:
        key = row["variant"], row["argument"], row["attempt"]
        if (
            key in keys
            or row["actual"] != expected(key[0], key[1])
            or row["returncode"]
        ):
            raise ValueError("duplicate or failed observation")
        if not row["entry_address"]:
            raise ValueError("no actual process address")
        log = json.loads((root / f"{key[0]}-{key[1]}-{key[2]}.json").read_text())
        fields = log["stdout"].strip().split()
        if (
            log["returncode"] != row["returncode"]
            or len(fields) != 7
            or list(map(int, fields[:6])) != row["actual"]
            or int(fields[6], 16) != row["entry_address"]
            or process_image_name(log["command"][0], report["system"])
            != report["images"][key[0]]["path"]
            or log["command"][1:] != [str(key[1])]
        ):
            raise ValueError("process log differs from observation")
        keys.add(key)
    if keys != {
        (v, x, n) for v in ("original", *VARIANTS) for x in ARGUMENTS for n in range(3)
    }:
        raise ValueError("incomplete execution inventory")
    if set(report["loader_rejections"]) != {"missing-library", "missing-symbol"}:
        raise ValueError("incomplete negative loader inventory")
    for name, row in report["loader_rejections"].items():
        log = json.loads((root / (name + "-process.json")).read_text())
        if log["returncode"] != row["returncode"] or not loader_rejected(
            report["system"], name, log
        ):
            raise ValueError("not a native loader rejection")
        if digest(root / row["path"]) != row["sha256"]:
            raise ValueError("negative image digest mismatch")
    for variant in VARIANTS:
        receipt = json.loads(
            (root / "exceptions" / (variant + "-receipt.json")).read_text()
        )
        validate_receipt(receipt, report["system"], report["architecture"], variant)
    if audit_exceptions(root / "exceptions", report["system"]) != report["exceptions"]:
        raise ValueError("exception subreport differs from manifest")
    return report


def validate_receipt(receipt, system, architecture, variant):
    cpu = {"x64": "x86_64", "arm64": "aarch64"}[architecture]
    target = receipt.get("target", "")
    pattern = {
        "Linux": cpu + r"-unknown-linux-gnu",
        "Windows": cpu + r"-pc-windows-msvc(?:[0-9.]+)?",
        "Darwin": ("arm64" if architecture == "arm64" else cpu)
        + r"-apple-macosx?[0-9.]+",
    }[system]
    if (
        not re.fullmatch(pattern, target)
        or receipt.get("schema") != "neverd.source-patch.v1"
        or receipt.get("artifact") != int(variant.endswith("-object"))
        or receipt.get("input_kind") != 0
    ):
        raise ValueError("receipt target or artifact differs from native qualification")


def exception_expected(variant: str, x: int) -> list[int]:
    if x < 0 and not variant.startswith("dependency-"):
        return [1, x - 10, 1]
    if x == 0 and variant.startswith(("dependency-", "combined-")):
        return [1, -20, 1]
    value = x + 4
    if variant.startswith("callback-"):
        value += 10
    if variant.startswith("dependency-"):
        value = 2 * x + 25
    if variant.startswith("combined-"):
        value = 3 * x + 39
    return [0, value, 1]


def qualify_exceptions(root, clang, flags, session, system):
    directory = root / "exceptions"
    directory.mkdir()
    export = (
        "__declspec(dllexport)"
        if system == "Windows"
        else '__attribute__((visibility("default")))'
    )
    prefix = "" if system == "Windows" else "lib"
    suffix = (
        ".dll" if system == "Windows" else ".dylib" if system == "Darwin" else ".so"
    )
    providers = {}
    for kind, symbol, body in (
        ("old", "old_throw", "if(x<0) throw x-10; return x+3;"),
        ("new", "dependency_throw", "if(x==0) throw -20; return 2*x+5;"),
    ):
        path = directory / (prefix + "exception_" + kind + suffix)
        source = directory / (kind + ".cpp")
        source.write_text(f'extern "C" {export} int {symbol}(int x) {{{body}}}\n')
        command = [
            clang,
            "--driver-mode=g++",
            *flags,
            "-dynamiclib" if system == "Darwin" else "-shared",
        ]
        if system != "Windows":
            command += ["-fPIC"]
        command += [str(source), "-o", str(path)]
        if system == "Darwin":
            command += ["-Wl,-install_name,@rpath/" + path.name]
        if system == "Linux":
            command += ["-Wl,-soname," + path.name]
        if system == "Windows":
            command += [
                "-Wl,/implib:" + str(directory / ("exception_" + kind + ".lib"))
            ]
        run(command, directory / (kind + "-build.json"))
        providers[kind] = {"path": path.name, "sha256": digest(path)}
    source = directory / "original.cpp"
    source.write_text(r"""
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#define IMPORT __declspec(dllimport)
#else
#define EXPORT __attribute__((visibility("default")))
#define IMPORT
#endif
extern "C" IMPORT int old_throw(int);
extern "C" EXPORT __attribute__((noinline)) int target(int x) {
  volatile int y=x; return old_throw(y)+1;
}
int cleanups=0;
struct Guard { ~Guard() { ++cleanups; } };
__attribute__((noinline)) int invoke(int x) { Guard g; return target(x); }
int main(int argc, char **argv) {
  if(argc!=2) return 99;
  int caught=0, result=0;
  try { result=invoke(atoi(argv[1])); }
  catch(int value) { caught=1; result=value; }
  printf("%d %d %d\n",caught,result,cleanups);
}
""")
    original = directory / ("original.exe" if system == "Windows" else "original")
    command = [
        clang,
        "--driver-mode=g++",
        *flags,
        str(source),
        "-o",
        str(original),
        "-L" + str(directory),
    ]
    if system == "Windows":
        command += [str(directory / "exception_old.lib")]
    else:
        command += ["-lexception_old", "-Wl,-rpath," + str(directory)]
    if system == "Linux":
        command += ["-Wl,--export-dynamic", "-Wl,--eh-frame-hdr", "-fPIE", "-pie"]
    run(command, directory / "original-build.json")
    session.load(original)
    matches = {
        int(r["addr"], 0) if isinstance(r["addr"], str) else r["addr"]
        for r in [*session.symbols, *session.exports]
        if r["name"] in ("target", "_target")
    }
    if len(matches) != 1:
        raise ValueError("missing exception fixture target")
    address = matches.pop()
    images = {"original": {"path": original.name, "sha256": digest(original)}}
    for variant in VARIANTS:
        callback = variant.startswith(("callback-", "combined-"))
        deps = not variant.startswith("callback-")
        expression = (
            "saved(x)+10"
            if variant.startswith("callback-")
            else "dependency_throw(x)+20"
            if variant.startswith("dependency-")
            else "saved(x)+dependency_throw(x)+30"
        )
        code = (
            'extern "C" int saved(int), dependency_throw(int);\nextern "C" int replacement(int x) {return '
            + expression
            + ";}\n"
        )
        if system == "Windows":
            code = code.replace(
                'extern "C" int saved(int), dependency_throw(int);',
                'extern "C" int saved(int);\n'
                'extern "C" __declspec(dllimport) int dependency_throw(int);',
            )
        library = providers["new"]["path"]
        if system == "Darwin":
            library = "@rpath/" + library
        path = directory / (variant + (".exe" if system == "Windows" else ""))
        receipt = session.patch_source(
            code,
            path,
            language="c++",
            mappings={"replacement": address},
            bindings={"saved": ("original", address)} if callback else {},
            dependencies={"dependency_throw": (library, "function")} if deps else {},
            artifact=variant.rsplit("-", 1)[1],
            compiler_path=clang,
            compiler_args=["-O0", "-fno-omit-frame-pointer"],
        )
        write_patch_source(directory / (variant + "-source.cpp"), code)
        (directory / (variant + "-receipt.json")).write_text(
            json.dumps(receipt, indent=2) + "\n"
        )
        images[variant] = {"path": path.name, "sha256": digest(path)}
    observations = []
    for variant, row in images.items():
        for x in ARGUMENTS:
            for attempt in range(3):
                log = directory / f"{variant}-{x}-{attempt}.json"
                result = run([str(directory / row["path"]), str(x)], log)
                actual = list(map(int, result.stdout.strip().split()))
                if actual != exception_expected(variant, x):
                    raise ValueError(f"exception unwind {variant}: {actual}")
                observations.append(
                    {
                        "variant": variant,
                        "argument": x,
                        "attempt": attempt,
                        "actual": actual,
                        "returncode": result.returncode,
                    }
                )
    report = {"images": images, "providers": providers, "observations": observations}
    (directory / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    audit_exceptions(directory, system)
    return report


def audit_exceptions(root, system):
    report = json.loads((root / "report.json").read_text())
    if set(report["images"]) != {"original", *VARIANTS}:
        raise ValueError("incomplete exception image inventory")
    for row in [*report["images"].values(), *report["providers"].values()]:
        if digest(root / row["path"]) != row["sha256"]:
            raise ValueError("exception image digest mismatch")
    for variant in VARIANTS:
        receipt = json.loads((root / (variant + "-receipt.json")).read_text())
        if (
            receipt.get("success") is not True
            or receipt["source_sha256"] != digest(root / (variant + "-source.cpp"))
            or len(receipt["installed"]) != 1
            or len(receipt["original_callbacks"])
            != int(variant.startswith(("callback-", "combined-")))
            or len(receipt["added_imports"]) != int(not variant.startswith("callback-"))
        ):
            raise ValueError("incomplete exception publication receipt")
    keys = set()
    for row in report["observations"]:
        key = row["variant"], row["argument"], row["attempt"]
        log = json.loads((root / f"{key[0]}-{key[1]}-{key[2]}.json").read_text())
        if (
            key in keys
            or row["actual"] != exception_expected(key[0], key[1])
            or row["returncode"]
            or log["returncode"]
            or list(map(int, log["stdout"].strip().split())) != row["actual"]
            or process_image_name(log["command"][0], system)
            != report["images"][key[0]]["path"]
            or log["command"][1:] != [str(key[1])]
        ):
            raise ValueError("invalid exception process observation")
        keys.add(key)
    if keys != {
        (v, x, n) for v in ("original", *VARIANTS) for x in ARGUMENTS for n in range(3)
    }:
        raise ValueError("incomplete exception execution inventory")
    return report


def loader_rejected(system: str, name: str, log: dict) -> bool:
    if not log["returncode"]:
        return False
    if system == "Windows":
        return log["returncode"] & 0xFFFFFFFF == (
            0xC0000135 if name == "missing-library" else 0xC0000139
        )
    text = log["stderr"].lower()
    return (
        ("library not loaded" in text or "error while loading shared libraries" in text)
        if name == "missing-library"
        else ("symbol not found" in text or "undefined symbol" in text)
    )


def qualify(args: argparse.Namespace) -> None:
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    system = platform.system()
    arch = native_architecture(platform.machine())
    if arch != args.expected_arch or system not in ("Darwin", "Linux", "Windows"):
        raise ValueError(f"wrong native host {system}/{platform.machine()}")
    if system == "Darwin":
        translated = subprocess.run(
            ["sysctl", "-in", "sysctl.proc_translated"], capture_output=True, text=True
        )
        if translated.stdout.strip() == "1":
            raise ValueError("Rosetta is not native hardware qualification")
    commit = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
    ).strip()
    if args.source_commit and commit != args.source_commit:
        raise ValueError("source checkout does not match qualification commit")
    dirty = subprocess.check_output(
        ["git", "status", "--porcelain", "--untracked-files=no"], cwd=ROOT, text=True
    )
    if args.source_commit and dirty:
        raise ValueError("qualification source contains tracked modifications")
    clang = str(args.clang.resolve())
    flags = ["-O0", "-fno-omit-frame-pointer", "-funwind-tables"]
    if system == "Windows":
        flags += [
            "--target="
            + ("aarch64" if arch == "arm64" else "x86_64")
            + "-pc-windows-msvc"
        ]
    if system == "Darwin":
        sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
        flags += ["-isysroot", sdk]
    prefix = "" if system == "Windows" else "lib"
    suffix = (
        ".dll" if system == "Windows" else ".dylib" if system == "Darwin" else ".so"
    )
    providers = {}
    for kind in ("old", "new"):
        stem = f"{prefix}patch_{kind}"
        path, source = root / (stem + suffix), root / (kind + ".c")
        export = "__declspec(dllexport) " if system == "Windows" else ""
        body = (
            "int old_add(int x) {return x+3;}\nEXPORT int old_data=17;"
            if kind == "old"
            else "int dependency_fn(int x) {return x*5+2;}\nEXPORT int dependency_data=37;"
        )
        source.write_text(export + body.replace("EXPORT ", export))
        command = [clang, *flags, "-dynamiclib" if system == "Darwin" else "-shared"]
        if system != "Windows":
            command += ["-fPIC"]
        command += [str(source), "-o", str(path)]
        if system == "Darwin":
            command += ["-Wl,-install_name,@rpath/" + path.name]
        if system == "Linux":
            command += ["-Wl,-soname," + path.name]
        if system == "Windows":
            command += ["-Wl,/implib:" + str(root / (stem + ".lib"))]
        run(command, root / (kind + "-build.json"))
        providers[kind] = {"path": path.name, "sha256": digest(path)}
    original = root / ("original.exe" if system == "Windows" else "original")
    source = root / "original.c"
    source.write_text(ORIGINAL)
    command = [clang, *flags, str(source), "-o", str(original), "-L" + str(root)]
    if system == "Windows":
        command += [str(root / "patch_old.lib")]
    else:
        command += ["-lpatch_old", "-Wl,-rpath," + str(root)]
    if system == "Linux":
        command += ["-Wl,--export-dynamic", "-Wl,--eh-frame-hdr", "-fPIE", "-pie"]
    if system == "Darwin":
        # Intel's linker does not sign by default. The original input must meet
        # the same strict signature check as every published Mach-O image.
        command += ["-Wl,-keep_dwarf_unwind", "-Wl,-adhoc_codesign"]
    run(command, root / "original-build.json")
    original_hash = digest(original)
    host = HostAPI(ctypes.CDLL(str(args.library.resolve())))
    handle = host.function("neverd_session_create")()
    if not handle:
        raise RuntimeError("cannot create NeverD session")

    class Bridge:
        @staticmethod
        def session_address(value):
            return value

    session = Session(handle, _native=Bridge(), _host=host)
    loader_rejections = {}
    images = {"original": {"path": original.name, "sha256": original_hash}}
    try:
        session.load(original)
        symbols = [*session.symbols, *session.exports]
        addresses = {}
        for name in ("target", "second", "target_fp", "stateful"):
            matches = {
                int(r["addr"], 0) if isinstance(r["addr"], str) else r["addr"]
                for r in symbols
                if r["name"] in (name, "_" + name)
            }
            if len(matches) != 1:
                raise ValueError(f"missing/ambiguous symbol {name}")
            addresses[name] = matches.pop()
        for variant in VARIANTS:
            callback = variant.startswith(("callback-", "combined-"))
            deps = not variant.startswith("callback-")
            mappings = {"replacement": addresses["target"]}
            bindings = {}
            if callback:
                bindings["saved"] = ("original", addresses["target"])
                for replacement, saved, name in (
                    ("replace_second", "saved_second", "second"),
                    ("replace_fp", "saved_fp", "target_fp"),
                    ("replace_state", "saved_state", "stateful"),
                ):
                    mappings[replacement] = addresses[name]
                    bindings[saved] = ("original", addresses[name])
            library = providers["new"]["path"]
            if system == "Darwin":
                library = "@rpath/" + library
            dependencies = (
                {
                    "dependency_fn": (library, "function"),
                    "dependency_data": (library, "data"),
                }
                if deps
                else {}
            )
            path = root / (variant + (".exe" if system == "Windows" else ""))
            code = (
                CALLBACKS
                if variant.startswith("callback-")
                else DEPENDENCY
                if variant.startswith("dependency-")
                else COMBINED
            )
            if system == "Windows":
                # Native COFF objects name IAT storage explicitly. A direct
                # external symbol cannot stand in for a loader-owned slot.
                code = code.replace(
                    "extern int dependency_fn(int);",
                    "__declspec(dllimport) int dependency_fn(int);",
                ).replace(
                    "extern int dependency_data;",
                    "__declspec(dllimport) extern int dependency_data;",
                )
            write_patch_source(root / (variant + "-source.c"), code)
            receipt = session.patch_source(
                code,
                path,
                mappings=mappings,
                bindings=bindings,
                dependencies=dependencies,
                artifact=variant.rsplit("-", 1)[1],
                compiler_path=clang,
                compiler_args=["-O0", "-fno-omit-frame-pointer"],
            )
            if len(receipt["original_callbacks"]) != (4 if callback else 0) or len(
                receipt["added_imports"]
            ) != (2 if deps else 0):
                raise ValueError("publication receipt has incomplete callbacks/imports")
            (root / (variant + "-receipt.json")).write_text(
                json.dumps(receipt, indent=2) + "\n"
            )
            if digest(original) != original_hash:
                raise ValueError("original input changed")
            images[variant] = {"path": path.name, "sha256": digest(path)}
        for name in ("missing-library", "missing-symbol"):
            library = (
                providers["new"]["path"]
                if name == "missing-symbol"
                else prefix + "absent_neverd_provider" + suffix
            )
            if system == "Darwin":
                library = "@rpath/" + library
            symbol = (
                "dependency_fn" if name == "missing-library" else "absent_neverd_symbol"
            )
            code = DEPENDENCY.replace("dependency_fn", symbol)
            path = root / (name + (".exe" if system == "Windows" else ""))
            receipt = session.patch_source(
                code,
                path,
                mappings={"replacement": addresses["target"]},
                dependencies={
                    symbol: (library, "function"),
                    "dependency_data": (library, "data"),
                },
                compiler_path=clang,
                compiler_args=["-O0", "-fno-omit-frame-pointer"],
            )
            (root / (name + "-receipt.json")).write_text(
                json.dumps(receipt, indent=2) + "\n"
            )
            write_patch_source(root / (name + "-source.c"), code)
            result = run(
                [str(path), "7"], root / (name + "-process.json"), allow_failure=True
            )
            log = json.loads((root / (name + "-process.json")).read_text())
            if not loader_rejected(system, name, log):
                raise ValueError(f"{name}: expected explicit OS loader rejection")
            loader_rejections[name] = {
                "path": path.name,
                "sha256": digest(path),
                "returncode": result.returncode,
            }
        exceptions = qualify_exceptions(root, clang, flags, session, system)
    finally:
        host.function("neverd_session_destroy")(handle)
    observations = []
    for variant, image in images.items():
        if system == "Darwin":
            valid = run(
                ["/usr/bin/dyld_info", "-validate_only", str(root / image["path"])],
                root / (variant + "-dyld.json"),
            )
            if len([line for line in valid.stdout.splitlines() if line.strip()]) != 1:
                raise ValueError(
                    "dyld rejected the native Mach-O structure: " + valid.stdout
                )
            run(
                [
                    "/usr/bin/codesign",
                    "--verify",
                    "--strict",
                    str(root / image["path"]),
                ],
                root / (variant + "-signature.json"),
            )
        for x in ARGUMENTS:
            for attempt in range(3):
                result = run(
                    [str(root / image["path"]), str(x)],
                    root / f"{variant}-{x}-{attempt}.json",
                )
                fields = result.stdout.strip().split()
                if len(fields) != 7:
                    raise ValueError("unexpected process output")
                actual = list(map(int, fields[:6]))
                if actual != expected(variant, x):
                    raise ValueError(f"{variant}: {actual} != {expected(variant, x)}")
                observations.append(
                    {
                        "variant": variant,
                        "argument": x,
                        "attempt": attempt,
                        "actual": actual,
                        "returncode": result.returncode,
                        "entry_address": int(fields[6], 16),
                    }
                )
    report = {
        "schema": SCHEMA,
        "source_commit": commit,
        "source_dirty": bool(dirty),
        "harness_commit": os.environ.get("GITHUB_SHA"),
        "run_id": os.environ.get("GITHUB_RUN_ID"),
        "run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
        "execution": "native-os-process",
        "system": system,
        "architecture": arch,
        "kernel": platform.release(),
        "engine_sha256": digest(args.library),
        "images": images,
        "providers": providers,
        "observations": observations,
        "loader_rejections": loader_rejections,
        "exceptions": exceptions,
        "evidence_files": {
            p.relative_to(root).as_posix(): digest(p)
            for p in sorted(root.rglob("*"))
            if p.is_file()
        },
    }
    (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    audit(root)
    print(
        f"{system}/{arch}: {len(images)} images, {len(observations)} native process observations passed"
    )


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--audit", type=Path)
    p.add_argument("--library", type=Path)
    p.add_argument("--clang", type=Path)
    p.add_argument("--output", type=Path)
    p.add_argument("--expected-arch", choices=("x64", "arm64"))
    p.add_argument("--source-commit")
    args = p.parse_args()
    if args.audit:
        report = audit(args.audit)
        if args.source_commit and (
            report["source_commit"] != args.source_commit or report["source_dirty"]
        ):
            raise ValueError("native report belongs to a different or dirty source")
        if args.expected_arch and report["architecture"] != args.expected_arch:
            raise ValueError("native report belongs to a different host")
        print(
            f"audited {len(report['observations'])} functional and {len(report['exceptions']['observations'])} exception observations"
        )
    else:
        if any(
            getattr(args, k) is None
            for k in ("library", "clang", "output", "expected_arch")
        ):
            p.error("qualification requires library, clang, output, and expected-arch")
        qualify(args)


if __name__ == "__main__":
    main()
