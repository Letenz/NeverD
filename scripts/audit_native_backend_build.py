#!/usr/bin/env python3
"""Audit actual ARM64 backend objects; this does not execute a virtual CPU."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess

try:
    from .probe_native_host import host_matches
except ImportError:
    from probe_native_host import host_matches

ROOT = Path(__file__).resolve().parents[1]
INVENTORY = ROOT / "scripts/NativeBackendBuild.def"


def definitions() -> tuple[dict, dict, dict, dict]:
    data = INVENTORY.read_text(encoding="utf-8")
    profiles = {row[0]: row[1:] for row in re.findall(
        r'NEVERD_NATIVE_BUILD_PROFILE\(\s*(\w+),\s*"([^"]+)",'
        r'\s*"([^"]+)",\s*"([^"]+)"\s*\)', data)}
    sources: dict[str, list[str]] = {}
    for backend, source in re.findall(
            r'NEVERD_NATIVE_BUILD_SOURCE\(\s*(\w+),\s*"([^"]+)"\s*\)', data):
        sources.setdefault(backend, []).append(source)
    values = {name: int(value, 0) for name, value in re.findall(
        r'NEVERD_NATIVE_BUILD_VALUE\(\s*(\w+),\s*(\w+)\s*\)', data)}
    texts = dict(re.findall(
        r'NEVERD_NATIVE_BUILD_TEXT\(\s*(\w+),\s*"([^"]+)"\s*\)', data))
    if (not profiles or set(profiles) != set(sources) or not values or not texts
            or any(len(rows) != len(set(rows)) for rows in sources.values())):
        raise ValueError(texts.get("InvalidInventory"))
    return profiles, sources, values, texts


def validate_object(data: bytes, backend: str) -> None:
    _, _, values, texts = definitions()

    def word(offset: int) -> int:
        return int.from_bytes(data[offset:offset + values["WordBytes"]], "little")

    if backend == "kvm":
        valid = (len(data) >= values["ELFHeaderSize"]
                 and data.startswith(bytes.fromhex(texts["ELFMagic"]))
                 and data[values["ELFClassOffset"]] == values["ELF64"]
                 and data[values["ELFDataOffset"]] == values["ELFLittleEndian"]
                 and word(values["ELFTypeOffset"]) == values["ELFRelocatable"]
                 and word(values["ELFMachineOffset"]) == values["ELFARM64"])
    elif backend == "whp":
        valid = (len(data) >= values["COFFHeaderSize"]
                 and word(0) == values["COFFARM64"]
                 and word(values["COFFOptionalSizeOffset"]) == 0)
        if not valid and len(data) >= values["BigObjHeaderSize"]:
            magic = bytes.fromhex(texts["BigObjMagic"])
            offset = values["BigObjMagicOffset"]
            valid = (word(0) == 0
                     and word(values["WordBytes"]) == values["BigObjSignature"]
                     and word(values["WordBytes"] * 2) >= values["BigObjVersion"]
                     and word(values["BigObjMachineOffset"]) == values["COFFARM64"]
                     and data[offset:offset + len(magic)] == magic)
    else:
        valid = False
    if not valid:
        raise ValueError(texts["InvalidObject"])


def audit_objects(build: Path, backend: str, root: Path = ROOT) -> list[dict]:
    build, root = build.resolve(), root.resolve()
    profiles, sources, _, texts = definitions()
    _, definition, suffix = profiles[backend]
    cache = dict(re.findall(r'^([^#/:][^:]*):[^=]+=(.*)$',
                           (build / texts["Cache"]).read_text(encoding="utf-8"),
                           re.M))
    if (cache.get(texts["CPUOption"]) != "ON"
            or cache.get(texts["UnicornOption"]) != "OFF"
            or cache.get(texts["BackendOption"] + backend.upper()) != "ON"):
        raise ValueError(texts["InvalidProfile"])
    commands = json.loads((build / texts["CompileCommands"]).read_text(encoding="utf-8"))
    objects = []
    for relative in sources[backend]:
        source = (root / texts["SourceRoot"] / relative).resolve()
        rows = [row for row in commands if Path(row["file"]).resolve() == source]
        if len(rows) != 1:
            raise ValueError(texts["MissingRecipe"])
        row = rows[0]
        command = row.get("command") or " ".join(row["arguments"])
        if not re.search(r'(?:^|\s)[-/]D' + re.escape(definition) + r'=1(?:\s|$)', command):
            raise ValueError(texts["MissingDefinition"])
        output = (build / texts["ObjectRoot"] / (relative + suffix)).resolve()
        if "output" in row:
            declared = Path(row["output"])
            if not declared.is_absolute():
                declared = Path(row["directory"]) / declared
            if declared.resolve() != output:
                raise ValueError(texts["WrongOutput"])
        data = output.read_bytes()
        validate_object(data, backend)
        objects.append({"source": source.relative_to(root).as_posix(),
                        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                        "object": str(output), "bytes": len(data),
                        "object_sha256": hashlib.sha256(data).hexdigest(),
                        "compile_command": command})
    return objects


def main() -> None:
    profiles, _, _, texts = definitions()
    parser = argparse.ArgumentParser(description=texts["Description"])
    parser.add_argument("--backend", choices=sorted(profiles), required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if (not host_matches(args.backend, "ARM64")
            or platform.system() != profiles[args.backend][0]):
        raise ValueError(texts["HostMismatch"])
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    if subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True).strip():
        raise ValueError(texts["DirtySource"])
    report = {"commit": commit, "backend": args.backend,
              "host": {"system": platform.system(), "machine": platform.machine()},
              "guest_execution_verified": False,
              "objects": audit_objects(args.build.resolve(), args.backend)}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"backend": args.backend, "objects": len(report["objects"]),
                      "guest_execution_verified": False}))


if __name__ == "__main__":
    main()
