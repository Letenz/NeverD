#!/usr/bin/env python3
"""Generate the CI dialect corpus with the built CLI and native ELF fixtures."""

import argparse
from pathlib import Path
import struct
import subprocess
import sys


def native_elf(arm64: bool) -> tuple[bytes, int]:
    # mov w0/eax, 7; ret. These are decoded as the guest ISA on every CI host.
    code = bytes.fromhex("e0008052c0035fd6" if arm64 else "b807000000c3")
    base = 0xFFFF800000400000 if arm64 else 0x400000
    entry = base + 120
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    header = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 183 if arm64 else 62, 1,
                         entry, 64, 0, 0, 64, 56, 1, 64, 0, 0)
    segment = struct.pack("<IIQQQQQQ", 1, 5, 0, base, base,
                          120 + len(code), 120 + len(code), 4096)
    return header + segment + code, entry


def prepare(neverd: Path, output: Path) -> None:
    neverd = neverd.resolve()
    if sys.platform == "win32" and not neverd.is_file():
        neverd = neverd.with_suffix(".exe")
    if not neverd.is_file():
        raise FileNotFoundError(f"built NeverD CLI is missing: {neverd}")
    output = output.resolve()
    corpus = output / "corpus"
    corpus.mkdir(parents=True, exist_ok=True)
    for arm64 in (False, True):
        name = "arm64" if arm64 else "x64"
        data, entry = native_elf(arm64)
        binary = output / f"{name}.elf"
        binary.write_bytes(data)
        for stage in ("c", "llvmc"):
            source = corpus / f"{name}-{stage}.c"
            # A successful command must create fresh output, even on a rerun.
            source.unlink(missing_ok=True)
            command = [str(neverd), "decompile", str(binary), "--no-debug",
                       "--func", hex(entry), "-o", str(source)]
            if stage == "llvmc":
                command.append("--llvm")
            result = subprocess.run(command, capture_output=True, text=True,
                                    timeout=120)
            log = result.stdout + result.stderr
            (output / f"{name}-{stage}.log").write_text(log, encoding="utf-8")
            if result.returncode:
                raise RuntimeError(f"{command!r} failed ({result.returncode}):\n{log}")
            if not source.is_file() or not source.read_text(encoding="utf-8").strip():
                raise RuntimeError(f"decompilation produced no C source: {source}")
            print(f"generated {source.name}: {source.stat().st_size} bytes")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args()
    prepare(arguments.neverd, arguments.output)


if __name__ == "__main__":
    main()
