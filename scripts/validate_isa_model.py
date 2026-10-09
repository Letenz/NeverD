#!/usr/bin/env python3
"""Validate the binary-file ISA model through the engine.

Each input is a real program or library with its ELF magic cleared, so the
engine reads it as a file no header describes, the way a firmware dump or a
carved image reaches it.  neverd_identify_json must settle the instruction
set the file was built for -- or, for a set the model does not know, settle
none.  The inputs are the Debian packages isa_model_corpus.json marks
"test", which the model never trained on, and the packages
isa_model_validation.json lists: OpenWrt builds (musl, GCC -Os) of sets the
model knows, and builds for sets it does not (ARC, IA-64, and the MIPS16e
code OpenWrt builds its busybox as).  Each file is read whole, and again two
bytes in, where a set whose instructions align must report the offset they
start at.

Writes the outcome per file to lib/loader/Raw/ISAModel.json under
"validation" and exits nonzero when any file settles wrongly.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_isa_model as model  # noqa: E402

VALIDATION = Path(__file__).resolve().parent / "isa_model_validation.json"
SHIFT = 2


def extract(package, directory):
    """The package's files, unpacked once into directory/files."""
    files = directory / "files"
    if files.exists():
        return files
    files.mkdir()
    if package.suffix == ".ipk":
        with tarfile.open(package, "r:gz") as outer:
            member = next(m for m in outer.getmembers() if m.name.endswith("data.tar.gz"))
            (directory / "data.tar.gz").write_bytes(outer.extractfile(member).read())
        subprocess.run(["tar", "-xzf", str(directory / "data.tar.gz"), "-C", str(files)],
                       check=True)
    else:
        subprocess.run(["ar", "x", str(package)], cwd=directory, check=True)
        data = next(directory.glob("data.tar.*"))
        subprocess.run(["tar", "-xf", str(data), "-C", str(files)], check=True)
    return files


def largest_elf(files, machine):
    """The largest ELF file for machine (number, 64-bit, big-endian)."""
    found = None
    for path in files.rglob("*"):
        if path.is_symlink() or not path.is_file():
            continue
        with path.open("rb") as handle:
            head = handle.read(20)
        if len(head) < 20 or head[:4] != b"\x7fELF":
            continue
        big = head[5] == 2
        number = int.from_bytes(head[18:20], "big" if big else "little")
        if (number, head[4] == 2, big) != tuple(machine):
            continue
        if found is None or path.stat().st_size > found.stat().st_size:
            found = path
    return found


class Engine:
    def __init__(self, library):
        self.lib = ctypes.CDLL(str(library))
        self.lib.neverd_identify_json.restype = ctypes.c_void_p
        self.lib.neverd_identify_json.argtypes = [ctypes.c_char_p]
        self.lib.neverd_free_string.argtypes = [ctypes.c_void_p]

    def identify(self, path):
        pointer = self.lib.neverd_identify_json(str(path).encode())
        try:
            reply = json.loads(ctypes.string_at(pointer).decode())
        finally:
            self.lib.neverd_free_string(pointer)
        return next(row for row in reply["rows"] if row["loader"] == "binary")


def judge(row, expected, shift):
    """Whether the engine read the file right: the set and the offset."""
    settled = row.get("status") == "settled"
    if expected is None:
        return not settled and row.get("status") != "width_unclear"
    if not settled or not row["guesses"] or row["guesses"][0]["isa"] != expected:
        return False
    return row.get("code_offset", 0) == shift % max(1, row.get("code_unit", 1))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--engine", type=Path, required=True,
                        help="libneverd.so (or .dylib, .dll) to identify with")
    parser.add_argument("--work", type=Path, help="Keep and reuse the packages here")
    args = parser.parse_args()
    engine = Engine(args.engine.resolve())
    work = (args.work or Path(tempfile.mkdtemp(prefix="neverd-isa-validate-"))).resolve()

    inputs = []
    for row in json.loads(model.PACKAGES.read_text(encoding="utf-8")):
        if row["role"] == "test":
            isa, number, wide, big = model.DEBIAN_ARCHES[row["arch"]]
            inputs.append({**row, "isa": isa, "machine": [number, wide, big]})
    inputs += json.loads(VALIDATION.read_text(encoding="utf-8"))

    results, failed = [], 0
    scratch = Path(tempfile.mkdtemp(prefix="neverd-isa-files-", dir=work))
    for index, row in enumerate(inputs):
        name = row["url"].rsplit("/", 1)[1]
        directory = work / "validate" / row["arch"] / name.rsplit(".", 1)[0]
        directory.mkdir(parents=True, exist_ok=True)
        package = model.fetch_package(row, directory)
        path = largest_elf(extract(package, directory), row["machine"])
        if path is None:
            sys.exit(f"{name}: no ELF file for its machine")
        data = bytearray(path.read_bytes())
        data[:4] = bytes(4)
        for shift in (0, SHIFT):
            probe = scratch / f"{index}-{shift}.bin"
            probe.write_bytes(bytes(shift) + bytes(data))
            got = engine.identify(probe)
            probe.unlink()
            right = judge(got, row["isa"], shift)
            failed += not right
            top = got["guesses"][0]["isa"] if got.get("guesses") else None
            results.append({
                "package": row["url"], "sha256": row["sha256"],
                "file": str(path.relative_to(directory / "files")), "shift": shift,
                "expected": row["isa"], "note": row.get("note"),
                "status": got.get("status"), "isa": top,
                "code_offset": got.get("code_offset", 0), "wide_share": got.get("wide_share"),
                "right": right})
            print(f"{'ok ' if right else 'BAD'} {row['arch']:18s} {name[:44]:44s} +{shift} "
                  f"{got.get('status') or '-':13s} {top or '-':12s} expected {row['isa'] or 'none'}",
                  flush=True)

    provenance_path = model.OUTPUT / "ISAModel.json"
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    provenance["validation"] = {
        "script": "scripts/validate_isa_model.py",
        "inputs": results,
        "right": len(results) - failed,
        "total": len(results),
    }
    provenance_path.write_text(json.dumps(provenance, indent=1, sort_keys=True) + "\n",
                               encoding="utf-8")
    print(f"{len(results) - failed} of {len(results)} files read right")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
