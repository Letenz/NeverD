#!/usr/bin/env python3
"""Build, reconstruct and execute a real SEH3 PE32 function, including rebasing.

The runtime caller PC must belong to the installed generated section. Original
and rewritten images must both restore FS:[0] after repeated exception dispatch.
This is deliberately separate from original-corpus and callback-only evidence.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET

if __package__:
    from .check_windows_registration_eh import run_image
    from .check_windows_registration_frame import KERNEL32, MSVCRT, undecorate_kernel32_imports
else:
    from check_windows_registration_eh import run_image
    from check_windows_registration_frame import KERNEL32, MSVCRT, undecorate_kernel32_imports

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "unittests/lift/eh/fixtures"
OBSERVATION = re.compile(
    r"neverd-registration-rewrite: value=(\d+) caller=([0-9a-fA-F]{8}) "
    r"entry=([0-9a-fA-F]{8}) chain=(\d+) iterations=(\d+) trace=(\d+)\s*")
CASES = {"filter": (0, 0), "nested-finally": (1, 123),
         "continue-search": (2, 123), "continue-execution": (3, 1),
         "normal-finally": (4, 2), "cdecl-parameter": (5, 7),
         "cdecl-parameter-write": (6, 177)}
CASES.update({"eh4-" + name: value for name, value in tuple(CASES.items())})


class PE32:
    def __init__(self, data: bytes | bytearray):
        self.data = data
        if len(data) < 64 or data[:2] != b"MZ":
            raise ValueError("missing DOS header")
        pe = self.u32(0x3c)
        if data[pe:pe + 4] != b"PE\0\0" or self.u16(pe + 4) != 0x14c:
            raise ValueError("expected i386 PE32")
        self.optional = pe + 24
        if self.u16(self.optional) != 0x10b:
            raise ValueError("expected PE32 optional header")
        self.base = self.u32(self.optional + 28)
        self.sections = []
        for index in range(self.u16(pe + 6)):
            offset = self.optional + self.u16(pe + 20) + 40 * index
            name = bytes(data[offset:offset + 8]).rstrip(b"\0").decode("ascii")
            virtual_size, rva, raw_size, raw = struct.unpack_from("<4I", data, offset + 8)
            self.sections.append((name, rva, virtual_size, raw, raw_size))

    def u16(self, offset: int) -> int:
        return struct.unpack_from("<H", self.data, offset)[0]

    def u32(self, offset: int) -> int:
        return struct.unpack_from("<I", self.data, offset)[0]

    def raw(self, rva: int, size: int = 4) -> int:
        owners = [raw + rva - start for _, start, _, raw, count in self.sections
                  if start <= rva and rva - start + size <= count]
        if len(owners) != 1 or owners[0] + size > len(self.data):
            raise ValueError("RVA has no unique raw-backed owner")
        return owners[0]

    def directory(self, index: int) -> tuple[int, int]:
        return struct.unpack_from("<2I", self.data, self.optional + 96 + 8 * index)

    def entry(self, name: bytes = b"registration_entry") -> int:
        export, _ = self.directory(0)
        table = self.raw(export, 40)
        functions, names, ordinals = struct.unpack_from("<3I", self.data, table + 28)
        for index in range(self.u32(table + 24)):
            name_offset = self.raw(self.u32(self.raw(names + 4 * index)), 1)
            end = self.data.index(0, name_offset)
            if self.data[name_offset:end] == name:
                ordinal = self.u16(self.raw(ordinals + 2 * index, 2))
                if ordinal >= self.u32(table + 20):
                    raise ValueError("export ordinal is out of bounds")
                return self.u32(self.raw(functions + 4 * ordinal))
        raise ValueError(f"required export is missing: {name!r}")

    def relocation_fields(self) -> set[int]:
        reloc, size = self.directory(5)
        if not reloc or not size:
            raise ValueError("probe has no relocation contract")
        cursor, fields = 0, set()
        while cursor < size:
            page, block_size = struct.unpack_from(
                "<2I", self.data, self.raw(reloc + cursor, 8))
            if page & 0xfff or block_size < 8 or block_size % 4 or cursor + block_size > size:
                raise ValueError("invalid relocation block")
            for index in range((block_size - 8) // 2):
                item = self.u16(self.raw(reloc + cursor + 8 + 2 * index, 2))
                if item >> 12 == 0:
                    continue
                target = page + (item & 0xfff)
                if item >> 12 != 3 or target in fields:
                    raise ValueError("unsupported or duplicate HIGHLOW relocation")
                self.raw(target)
                fields.add(target)
            cursor += block_size
        if not fields:
            raise ValueError("probe has no HIGHLOW fields")
        return fields

    def rebase(self, new_base: int) -> bytes:
        data = bytearray(self.data)
        if new_base % 0x10000 or not 0 < new_base <= 0xffffffff:
            raise ValueError("rebasing requires relocations and an aligned image base")
        delta = new_base - self.base
        for target in self.relocation_fields():
            offset = self.raw(target)
            struct.pack_into("<I", data, offset, (self.u32(offset) + delta) & 0xffffffff)
        struct.pack_into("<I", data, self.optional + 28, new_base)
        struct.pack_into("<I", data, self.optional + 64, 0)
        return bytes(data)


def observe(image: Path, generated: bool, launcher: list[str],
            environment: dict[str, str], timeout: float, expected_trace: int) -> dict:
    pe = PE32(image.read_bytes())
    config_rva, config_size = pe.directory(10)
    config = pe.raw(config_rva, config_size)
    if config_size < 92 or pe.u32(config) < 92 or pe.u32(config + 88) != 0x400:
        raise ValueError("probe lost its declared Guard CF contract")
    table, count = pe.u32(config + 80), pe.u32(config + 84)
    fields = pe.relocation_fields()
    if generated:
        if not count or table < pe.base or config_rva + 80 not in fields:
            raise ValueError("new Guard CF table has no HIGHLOW pointer owner")
        pe.raw(table - pe.base, count * 4)
    elif table or count or config_rva + 80 in fields:
        raise ValueError("original Guard CF table must be empty and unrelocated")
    result = run_image(image, launcher, environment, timeout)
    match = OBSERVATION.fullmatch(result.get("stdout", ""))
    if result.get("exit_code") != 0 or not match:
        raise ValueError(f"exception execution failed: {result}")
    value, caller, entry, chain, iterations, trace = (int(v, 16 if i in (1, 2) else 10)
                                              for i, v in enumerate(match.groups()))
    runtime_base = entry - pe.entry()
    if ((value, chain, iterations, trace) != (7, 1, 4, expected_trace) or
            runtime_base != pe.base):
        raise ValueError("exception result, chain restoration or forced base differs")
    owners = [name for name, start, size, _, _ in pe.sections
              if start <= caller - runtime_base < start + size]
    if owners != ([".ndtext"] if generated else [".text"]):
        raise ValueError("runtime caller PC does not prove the required source/generated path")
    if generated:
        source = pe.raw(pe.entry(), 5)
        displacement = struct.unpack_from("<i", pe.data, source + 1)[0]
        target = pe.entry() + 5 + displacement
        if pe.data[source] != 0xe9 or not any(
                name == ".ndtext" and start <= target < start + size
                for name, start, size, _, _ in pe.sections):
            raise ValueError("source entry does not redirect into generated code")
    return {"image": str(image), "sha256": hashlib.sha256(pe.data).hexdigest(),
            "generated": generated, "runtime_base": runtime_base,
            "guard_cf_count": count, "guard_cf_pointer_relocated": config_rva + 80 in fields,
            "caller_rva": caller - runtime_base, "value": value,
            "chain_restored": bool(chain), "iterations": iterations,
            "trace": trace, "runtime": result}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", required=True, type=Path)
    parser.add_argument("--patch-binary", type=Path,
                        help="also execute the CLI section and inplace patch paths")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--compiler", default="clang")
    parser.add_argument("--linker", default="lld-link")
    parser.add_argument("--wine")
    parser.add_argument("--wine-prefix", type=Path)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--case", choices=CASES, default="filter")
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    case, expected_trace = CASES[args.case]
    eh4 = args.case.startswith("eh4-")
    report = {"schema": 2, "evidence": f"reconstructed-source-registration-{'seh4' if eh4 else 'seh3'}",
              "case": args.case, "expected_trace": expected_trace,
              "passed": False, "steps": [], "observations": []}
    environment = os.environ.copy()
    try:
        compiler, linker = shutil.which(args.compiler), shutil.which(args.linker)
        if not compiler or not linker:
            raise ValueError("cross-target clang and lld-link are required")
        launcher = []
        if os.name != "nt":
            wine = args.wine or shutil.which("wine") or shutil.which("wine64-stable")
            if not wine:
                raise ValueError("Wine is unavailable; reconstructed source was not executed")
            launcher = [wine]
            prefix = (args.wine_prefix or output / "wine-prefix").resolve()
            prefix.mkdir(parents=True, exist_ok=True)
            environment.update(WINEPREFIX=str(prefix), WINEDEBUG="-all")
        commands = []
        definitions = (("kernel32", KERNEL32),
                       ("msvcrt", MSVCRT + ("_except_handler4_common\n" if eh4 else "")))
        for name, definition in definitions:
            (output / f"{name}.def").write_text(definition, encoding="utf-8")
            commands.append([linker, "/lib", f"/def:{output / (name + '.def')}",
                             "/machine:x86", f"/out:{output / (name + '.lib')}"])
        sources = (["registration_seh4.s", "registration_eh4_runtime.c"] if eh4
                   else ["registration_seh3.s"])
        sources.append("registration_observer.c")
        for source in sources:
            commands.append([compiler, "--target=i686-pc-windows-msvc", "-O0",
                             f"-DREGISTRATION_CASE={case}",
                             "-DREGISTRATION_FORWARD_ONLY=1", "-DREGISTRATION_EXPECT_GS=0",
                             *( ["-x", "assembler-with-cpp"] if source.endswith(".s") else []),
                             "-fno-stack-protector", "-c", str(FIXTURES / source),
                             "-o", str(output / (source + ".obj"))])
        original, patched = output / "original.exe", output / "patched.exe"
        for stale in (original, patched, output / "product-patched.exe",
                      output / "collision-patched.exe",
                      output / "rewrite.xml"):
            stale.unlink(missing_ok=True)
        commands.append([linker, "/entry:mainCRTStartup", "/nodefaultlib",
                         "/machine:x86", "/subsystem:console", "/dynamicbase:no",
                         "/export:registration_entry",
                         *( ["/export:registration_eh4_personality=_except_handler4"] if eh4 else []),
                         f"/out:{original}",
                         *(str(output / (name + ".obj")) for name in sources),
                         str(output / "kernel32.lib"), str(output / "msvcrt.lib")])
        environment.update(NEVERD_REGISTRATION_INPUT_PE32=str(original),
                           NEVERD_REGISTRATION_OUTPUT_PE32=str(patched),
                           NEVERD_REGISTRATION_OUTPUT_IR=str(output / "registration.ll"))
        commands.append([str(args.test_binary.resolve()),
                         "--gtest_filter=WindowsRegistrationNative.InputPE32PreservesItsCheckedSourceContract",
                         f"--gtest_output=xml:{output / 'rewrite.xml'}"])
        for index, command in enumerate(commands):
            result = subprocess.run(command, env=environment, capture_output=True,
                                    text=True, errors="replace", timeout=args.timeout)
            report["steps"].append({"command": command, "exit_code": result.returncode,
                                    "stdout": result.stdout, "stderr": result.stderr})
            if result.returncode:
                raise ValueError(f"fixture build or reconstruction failed: {command[0]}")
            if index == 0:
                undecorate_kernel32_imports(output / "kernel32.lib")
        tests = ET.parse(output / "rewrite.xml").getroot()
        if int(tests.get("tests", "0")) != 1 or tests.findall(".//skipped"):
            raise ValueError("source reconstruction was not executed")
        images = [(original, False), (patched, True),
                  (output / "product-patched.exe", True),
                  (output / "collision-patched.exe", True)]
        if args.patch_binary:
            for mode in ("section", "inplace"):
                destination = output / f"cli-{mode}.exe"
                destination.unlink(missing_ok=True)
                command = [str(args.patch_binary.resolve()), "patch", str(original),
                           f"--from-ir={output / 'registration.ll'}", f"--mode={mode}",
                           "--no-opt", "-o", str(destination)]
                result = subprocess.run(command, env=environment, capture_output=True,
                                        text=True, errors="replace", timeout=args.timeout)
                report["steps"].append({"command": command, "exit_code": result.returncode,
                                        "stdout": result.stdout, "stderr": result.stderr})
                if result.returncode or not destination.is_file():
                    raise ValueError(f"CLI {mode} reconstruction failed")
                images.append((destination, True))
        for image, generated in images:
            report["observations"].append(observe(image, generated, launcher,
                                                 environment, args.timeout, expected_trace))
            rebased = image.with_name(image.stem + "-rebased.exe")
            rebased.write_bytes(PE32(image.read_bytes()).rebase(0x18000000))
            report["observations"].append(observe(rebased, generated, launcher,
                                                 environment, args.timeout, expected_trace))
        report["passed"] = True
    except (OSError, ValueError, struct.error, ET.ParseError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    destination = output / "registration-rewrite.json"
    destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{'PASS' if report['passed'] else 'FAIL'} reconstructed source: {destination}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
