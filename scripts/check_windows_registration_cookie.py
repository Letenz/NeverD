#!/usr/bin/env python3
"""Execute compiler-owned EH4 frames with strict positive/negative cookies.

Wine supplies the real EH4 dispatcher but currently omits cookie validation.
The fixture validates the documented runtime frame before forwarding to it.
Both a valid cookie and a deliberately damaged cookie are required evidence.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess

if __package__:
    from .check_windows_registration_eh import run_image
    from .check_windows_registration_frame import (
        BANNER, KERNEL32, MSVCRT, undecorate_kernel32_imports)
    from .check_windows_registration_rewrite import FIXTURES, PE32
else:
    from check_windows_registration_eh import run_image
    from check_windows_registration_frame import (
        BANNER, KERNEL32, MSVCRT, undecorate_kernel32_imports)
    from check_windows_registration_rewrite import FIXTURES, PE32


def require_cookie_outcome(result: dict, corrupted: bool) -> None:
    if corrupted:
        if result.get("exit_code") != 99 or result.get("stdout", "").strip():
            raise ValueError("damaged EH4 cookie did not terminate before dispatch")
    elif result.get("exit_code") != 0 or result.get("stdout", "").strip() != BANNER:
        raise ValueError("compiler-owned EH4 cookie or callback frame is incorrect")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--object", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--compiler", default="clang")
    parser.add_argument("--linker", default="lld-link")
    parser.add_argument("--wine")
    parser.add_argument("--wine-prefix", type=Path)
    parser.add_argument("--gs", action="store_true",
                        help="require a GS cookie and test its corruption separately")
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"schema": 1, "evidence": "generated-x86-eh4-strict-cookie-abi",
              "gs_required": args.gs, "passed": False, "steps": [], "observations": []}
    environment = os.environ.copy()
    try:
        compiler, linker = shutil.which(args.compiler), shutil.which(args.linker)
        if not compiler or not linker:
            raise ValueError("cross-target clang and lld-link are required")
        source = args.object.resolve()
        raw_object = source.read_bytes()
        if len(raw_object) < 20 or struct.unpack_from("<H", raw_object)[0] != 0x14c:
            raise ValueError("cookie probe must be an i386 COFF object")
        report["object_sha256"] = hashlib.sha256(raw_object).hexdigest()
        launcher = []
        if os.name != "nt":
            wine = args.wine or shutil.which("wine") or shutil.which("wine64-stable")
            if not wine:
                raise ValueError("Wine unavailable; EH4 cookies were not executed")
            launcher = [wine]
            prefix = (args.wine_prefix or output / "wine-prefix").resolve()
            prefix.mkdir(parents=True, exist_ok=True)
            environment.update(WINEPREFIX=str(prefix), WINEDEBUG="-all")
        commands = []
        definitions = (("kernel32", KERNEL32),
                       ("msvcrt", MSVCRT + "_except_handler4_common\n"))
        for name, definition in definitions:
            (output / f"{name}.def").write_text(definition, encoding="utf-8")
            commands.append([linker, "/lib", f"/def:{output / (name + '.def')}",
                             "/machine:x86", f"/out:{output / (name + '.lib')}"])
        for name in ("registration_eh4_runtime.c", "registration_eh4_config.s"):
            commands.append([compiler, "--target=i686-pc-windows-msvc", "-O0",
                             f"-DREGISTRATION_EXPECT_GS={int(args.gs)}",
                             "-fno-stack-protector", "-c", str(FIXTURES / name),
                             "-o", str(output / (name + ".obj"))])
        image = output / "valid.exe"
        commands.append([linker, "/entry:mainCRTStartup", "/nodefaultlib",
                         "/machine:x86", "/subsystem:console", "/dynamicbase:no",
                         "/export:registration_corrupt_cookie,DATA",
                         f"/out:{image}", str(source),
                         str(output / "registration_eh4_runtime.c.obj"),
                         str(output / "registration_eh4_config.s.obj"),
                         str(output / "kernel32.lib"), str(output / "msvcrt.lib")])
        for index, command in enumerate(commands):
            result = subprocess.run(command, env=environment, capture_output=True,
                                    text=True, errors="replace", timeout=args.timeout)
            report["steps"].append({"command": command, "exit_code": result.returncode,
                                    "stdout": result.stdout, "stderr": result.stderr})
            if result.returncode:
                raise ValueError(f"cookie probe build failed: {command[0]}")
            if index == 0:
                undecorate_kernel32_imports(output / "kernel32.lib")
        original = PE32(image.read_bytes())
        for base in (original.base, 0x18000000):
            valid = PE32(original.rebase(base))
            for corrupt in ((0, 1, 2) if args.gs else (0, 1)):
                data = bytearray(valid.data)
                offset = valid.raw(valid.entry(b"registration_corrupt_cookie"))
                struct.pack_into("<I", data, offset, int(corrupt))
                label = ("valid", "corrupt-eh", "corrupt-gs")[corrupt]
                path = output / f"{label}-{base:08x}.exe"
                path.write_bytes(data)
                result = run_image(path, launcher, environment, args.timeout)
                report["observations"].append({
                    "image": str(path), "sha256": hashlib.sha256(data).hexdigest(),
                    "image_base": base, "cookie_corrupted": label, "runtime": result})
                require_cookie_outcome(result, bool(corrupt))
        report["passed"] = True
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    (output / "cookie-runtime.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"{'PASS' if report['passed'] else 'FAIL'} strict EH4 cookies: {output / 'cookie-runtime.json'}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
