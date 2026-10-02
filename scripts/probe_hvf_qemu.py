#!/usr/bin/env python3
"""Compare an independent Intel HVF engine with a software execution control.

This diagnostic does not validate NeverD's CPU transport or Darwin contract.
The ROM is original code; QEMU is used only as a separate executable. See
https://www.qemu.org/docs/master/system/invocation.html for its accelerator CLI.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--trace", type=Path)
    args = parser.parse_args()
    root = args.evidence.resolve()
    root.mkdir(parents=True, exist_ok=True)
    qemu = shutil.which(args.qemu)
    if qemu is None:
        parser.error(f"QEMU executable not found: {args.qemu}")
    # Original BIOS: enter protected mode, initialize three identity page-table
    # pages, enter 64-bit mode, then return 37 via the debug-exit I/O port.
    # Assembled from probe_hvf_qemu_long_mode.s using Clang's ELF assembler and
    # ld.lld --image-base=0 -Ttext=0xf0000 --oformat=binary.
    rom = bytearray(65536)
    code = bytes.fromhex(
        "fafc2e660f0116c0000f20c06683c8010f22c066ea1b000f000800"
        "66b810008ed88ec08ed08ee08ee8bc00800000bf00100000b9000c000031c0f3ab"
        "c7050010000003200000c7050020000003300000c7050030000083000000"
        "b8200600000f22e0b8001000000f22d8b9800000c031d2b8000900000f30"
        "0f20c025ffffff9f0d330001800f22c0ea8f000f001800"
        "b82500000066ba0105eff4ebfe0f1f4000"
        "0000000000000000ffff0000009bcf00ffff00000093cf00ffff0000009baf00"
        "1f00a0000f00")
    rom[:len(code)] = code
    rom[0xfff0:0xfff5] = bytes.fromhex("ea000000f0")
    bios = root / "original-reset.bin"
    bios.write_bytes(rom)
    version = subprocess.run([qemu, "--version"], check=True, text=True,
                             capture_output=True, timeout=10).stdout
    summary = {"scope": "independent-qemu-host-diagnostic",
               "host": platform.platform(), "machine": platform.machine(),
               "qemu": qemu, "qemu_version": version.strip(),
               "rom_sha256": hashlib.sha256(rom).hexdigest(),
               "expected_exit_status": (37 << 1) | 1, "results": []}
    for accelerator in ("tcg", "hvf"):
        # A single explicit accelerator prevents silent fallback to software.
        command = [qemu, "-machine", "q35", "-accel", accelerator,
                   "-m", "64M", "-smp", "1", "-nodefaults", "-no-user-config",
                   "-display", "none", "-serial", "none", "-monitor", "none",
                   "-nic", "none", "-no-reboot", "-bios", str(bios),
                   "-device", "isa-debug-exit,iobase=0x501,iosize=4"]
        result = {"accelerator": accelerator, "command": command,
                  "passed": False, "timed_out": False}
        log = root / f"qemu-{accelerator}.log"
        with log.open("w") as output:
            try:
                environment = os.environ.copy()
                if args.trace and accelerator == "hvf":
                    environment["DYLD_INSERT_LIBRARIES"] = str(args.trace.resolve())
                process = subprocess.run(command, stdout=output,
                                         env=environment,
                                         stderr=subprocess.STDOUT, timeout=30)
                result["returncode"] = process.returncode
                result["passed"] = process.returncode == summary["expected_exit_status"]
            except subprocess.TimeoutExpired:
                result["timed_out"] = True
        result["log"] = log.name
        summary["results"].append(result)
        print(f"{accelerator}: {json.dumps(result)}", flush=True)
        print(log.read_text(errors="replace"), flush=True)
    summary["passed"] = all(result["passed"] for result in summary["results"])
    (root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
