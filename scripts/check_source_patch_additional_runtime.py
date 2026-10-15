#!/usr/bin/env python3
"""Qualify macOS and 32-bit source patches with explicit execution provenance."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from scripts import check_source_patch_cpp_runtime as cpp
from scripts import check_source_patch_pe_runtime as pe
from scripts import check_source_patch_snapshot_runtime as snapshot

SCHEMA = "neverd.source-patch.additional-runtime.v1"
PROFILES = {("macos", "x64"): 0x1000007, ("macos", "arm64"): 0x100000c,
            ("windows", "x86"): 0x14c, ("windows", "x64"): 0x8664,
            ("windows", "arm64"): 0xaa64, ("linux", "x86"): 3,
            ("linux", "arm32"): 40}


def abi_cases(gcc: bool = False, prefix: str = "abi-") -> dict:
    artifacts = pe.ARTIFACTS + (("gcc-source", "gcc-object") if gcc else ())
    return {prefix + "original": ((None, 9),),
            **{prefix + case + "-" + artifact: ((None, expected),)
               for case, (expected, _) in pe.CASES.items() for artifact in artifacts}}


def cases(system: str, arch: str) -> dict:
    if (system, arch) not in PROFILES:
        raise ValueError("unsupported additional runtime profile")
    if system == "macos":
        return {**abi_cases(), "original-eh": cpp.ORIGINAL_CASES,
                "original-callback": ((0, 1), (2, 42)),
                "c-ir": ((0, 11), (2, 15)), "c-object": ((0, 11), (2, 15)),
                "c-hosted": ((0, 11), (2, 15)), "hosted": ((0, 40), (2, 42)),
                "exceptions": cpp.PATCHED_CASES}
    if system == "windows" and arch != "x86":
        return {"original-eh": cpp.ORIGINAL_CASES, "exceptions": cpp.PATCHED_CASES}
    if arch == "arm32":
        return {**abi_cases(True, "arm-"), **abi_cases(True, "thumb-")}
    return abi_cases(system == "linux")


def machine(path: Path, system: str) -> int:
    if system == "windows":
        return pe.machine(path)
    data = path.read_bytes()
    if system == "macos":
        if len(data) < 32 or struct.unpack_from("<I", data)[0] != 0xfeedfacf:
            raise ValueError("expected a thin little-endian Mach-O64 image")
        if struct.unpack_from("<I", data, 12)[0] != 2:
            raise ValueError("expected a Mach-O executable")
        return struct.unpack_from("<I", data, 4)[0]
    if len(data) < 52 or data[:6] != b"\x7fELF\x01\x01":
        raise ValueError("expected a little-endian ELF32 image")
    if struct.unpack_from("<H", data, 16)[0] not in (2, 3):
        raise ValueError("expected an ELF32 executable")
    return struct.unpack_from("<H", data, 18)[0]


def macho_bytes(path: Path, va: int, size: int) -> bytes:
    machine(path, "macos")
    data = path.read_bytes()
    count, length = struct.unpack_from("<II", data, 16)
    if size <= 0 or 32 + length > len(data) or count > length // 8:
        raise ValueError("invalid Mach-O command inventory or code extent")
    position, matches = 32, []
    for _ in range(count):
        if position + 8 > 32 + length:
            raise ValueError("truncated Mach-O command")
        command, stride = struct.unpack_from("<II", data, position)
        if stride < 8 or position + stride > 32 + length:
            raise ValueError("invalid Mach-O command size")
        if command == 0x19:
            if stride < 72:
                raise ValueError("truncated Mach-O segment")
            begin, _, offset, filesz = struct.unpack_from("<QQQQ", data, position + 24)
            protection = struct.unpack_from("<I", data, position + 60)[0]
            if protection & 4 and begin <= va and va + size <= begin + filesz:
                start = offset + va - begin
                if start + size <= len(data):
                    matches.append(data[start:start + size])
        position += stride
    if len(matches) != 1:
        raise ValueError("hosted code has no unique executable Mach-O extent")
    return matches[0]


def runtime_identity(system: str, arch: str, runner: Path | None) -> dict:
    os_name = {"Darwin": "macos", "Linux": "linux", "Windows": "windows"}.get(platform.system())
    host = {"x86_64": "x64", "amd64": "x64", "aarch64": "arm64",
            "arm64": "arm64", "i686": "x86", "i386": "x86",
            "armv7l": "arm32"}.get(platform.machine().lower())
    if os_name != system or host is None:
        raise RuntimeError("runtime requires its declared operating system and known host")
    if system == "windows":
        host = {0x8664: "x64", 0xaa64: "arm64", 0x14c: "x86"}.get(pe.native_machine())
    elif system == "macos":
        # platform.machine() describes a translated Python process on Rosetta.
        result = subprocess.run(["/usr/sbin/sysctl", "-n", "hw.optional.arm64"],
                                capture_output=True, text=True, timeout=5)
        if result.returncode == 0 and result.stdout.strip() == "1":
            host = "arm64"
        elif host != "x64":
            raise RuntimeError("could not establish the physical macOS architecture")
    result = {"host_system": system, "host_architecture": host}
    if runner:
        expected = {"arm32": "qemu-arm", "x86": "qemu-i386"}.get(arch)
        if system != "linux" or runner.name != expected:
            raise ValueError("only the declared Linux QEMU user-mode runner is admitted")
        version = subprocess.run([str(runner), "--version"], capture_output=True,
                                 text=True, timeout=5, check=True)
        if "qemu" not in version.stdout.lower():
            raise ValueError("runtime runner identity mismatch")
        result.update(execution_kind="qemu-user", runner=runner.name,
                      runner_version=version.stdout.splitlines()[0])
    elif arch == host:
        result["execution_kind"] = "native"
    elif arch == "x86" and host == "x64" and system in ("windows", "linux"):
        result["execution_kind"] = "compatibility"
    elif arch == "x64" and host == "arm64" and system == "macos":
        result["execution_kind"] = "rosetta"
    else:
        raise RuntimeError("target cannot execute directly on this host")
    return result


def prepare(repository: Path, fixtures: Path, output: Path, system: str, arch: str) -> None:
    inventory = cases(system, arch)
    originals = frozenset(name for name in inventory if name.endswith("original") or
                          name in ("original-eh", "original-callback"))
    suffix = {"macos": ".macho", "windows": ".exe", "linux": ".elf"}[system]
    profile = snapshot.RuntimeProfile(SCHEMA, inventory, suffix,
        lambda path: machine(path, system), PROFILES[system, arch], originals)
    snapshot.prepare(repository, fixtures, output, system, arch, profile=profile,
                     producer_commit=None)


def execute(args: argparse.Namespace) -> dict:
    root = args.root.resolve()
    output = root / (args.arch + "-runtime.json")
    output.unlink(missing_ok=True)
    identity = runtime_identity(args.system, args.arch, args.runner)
    if identity["execution_kind"] != args.execution:
        raise RuntimeError("actual execution kind differs from required qualification")
    sysroot = getattr(args, "sysroot", None)
    if sysroot and (not args.runner or not sysroot.is_dir()):
        raise ValueError("a QEMU sysroot requires an explicit runner and existing directory")
    loader = getattr(args, "loader", None)
    if loader and (not args.runner or not loader.is_file()):
        raise ValueError("an explicit loader requires QEMU and an existing loader file")
    runner_args = ["-L", str(sysroot)] if sysroot else []
    if args.runner:
        identity["runner_arguments"] = runner_args
        if loader:
            identity["loader"] = str(loader)
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("schema") != SCHEMA:
        raise ValueError("unknown additional runtime schema")
    if os.environ.get("GITHUB_SHA") and manifest.get("commit") != os.environ["GITHUB_SHA"]:
        raise ValueError("runtime fixtures belong to a different commit")
    wanted = cases(args.system, args.arch)
    rows = manifest["images"]
    if (len(rows) != len(wanted) or {row["case"] for row in rows} != set(wanted) or
        any(row["architecture"] != args.arch for row in rows)):
        raise ValueError("missing, duplicate or foreign runtime image inventory")
    checked = []
    for row in rows:
        path = (root / row["path"]).resolve()
        if (not path.is_relative_to(root) or pe.digest(path) != row["sha256"] or
            machine(path, args.system) != PROFILES[args.system, args.arch]):
            raise ValueError("runtime image path, digest or machine mismatch")
        checked.append((row, path))
    observations = []
    for row, path in checked:
        if args.system != "windows":
            path.chmod(0o755)
        if args.system == "macos":
            pe.run(["/usr/bin/codesign", "--verify", "--strict", str(path)],
                   path.with_suffix(".codesign.log"))
        for mode, expected in wanted[row["case"]]:
            for attempt in range(3):
                command = ([str(args.runner), *runner_args] if args.runner else [])
                command += ([str(loader)] if loader else []) + [str(path)]
                if mode is not None:
                    command.append(str(mode))
                result = pe.run(command, path.with_suffix(f".mode-{mode}-{attempt}.log"), expected)
                observations.append({"case": row["case"], "mode": mode, "attempt": attempt,
                    "path": row["path"], "sha256": row["sha256"],
                    "expected": expected, "actual": result.returncode})
    report = {"schema": SCHEMA, "commit": manifest["commit"],
        "engine_source_commit": manifest["engine_source_commit"],
        "engine_source_digest": manifest["engine_source_digest"],
        "producer_commit": manifest["producer_commit"], "system": args.system,
        "architecture": args.arch, **identity, "observations": observations}
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{args.system}/{args.arch}: {len(observations)} process observations passed "
          f"({identity['execution_kind']}, host {identity['host_architecture']})")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--system", choices=("macos", "windows", "linux"), required=True)
    parser.add_argument("--arch", choices=("x86", "x64", "arm32", "arm64"), required=True)
    parser.add_argument("--execution", choices=("native", "compatibility", "qemu-user", "rosetta"), required=True)
    parser.add_argument("--runner", type=Path)
    parser.add_argument("--sysroot", type=Path)
    parser.add_argument("--loader", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repository = Path(__file__).resolve().parents[1]
    fixtures = repository / "unittests/Inputs/source-patch/additional-processes"
    prepare(repository, fixtures, args.output.resolve(), args.system, args.arch)
    args.root = args.output
    execute(args)


if __name__ == "__main__":
    main()
