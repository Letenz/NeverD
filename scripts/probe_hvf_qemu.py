#!/usr/bin/env python3
"""Compare an independent Intel HVF engine with a software execution control.

This diagnostic does not validate NeverD's CPU transport or Darwin contract.
The ROM is original code; QEMU is used only as a separate executable. See
https://www.qemu.org/docs/master/system/invocation.html for its accelerator CLI.
"""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    root = args.evidence.resolve()
    root.mkdir(parents=True, exist_ok=True)
    qemu = shutil.which(args.qemu)
    if qemu is None:
        parser.error(f"QEMU executable not found: {args.qemu}")
    # Original reset-vector program: MOV EAX,37; MOV DX,0x501; OUT DX,EAX;
    # HLT; JMP to itself. Operand-size prefixes select EAX in 16-bit mode.
    rom = bytearray(b"\xff" * 65536)
    code = bytes.fromhex("66 b8 25 00 00 00 ba 01 05 66 ef f4 eb fe")
    rom[0xfff0:0xfff0 + len(code)] = code
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
                process = subprocess.run(command, stdout=output,
                                         stderr=subprocess.STDOUT, timeout=15)
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
