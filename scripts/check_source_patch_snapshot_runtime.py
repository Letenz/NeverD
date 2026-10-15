#!/usr/bin/env python3
"""Replay authenticated current-engine images on native Windows/Linux hosts."""
from __future__ import annotations

import argparse
import base64
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Callable
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from scripts import check_source_patch_elf_runtime as elf
from scripts import check_source_patch_hosted_pe_runtime as hosted
from scripts import check_source_patch_pe_runtime as pe

SCHEMA = "neverd.source-patch.native-snapshot.v1"


@dataclass(frozen=True)
class RuntimeProfile:
    schema: str
    cases: dict
    suffix: str
    machine: Callable[[Path], int]
    machine_id: int
    originals: frozenset[str]


def decode_bounded(encoded: str, limit: int) -> bytes:
    decoder = zlib.decompressobj()
    payload = decoder.decompress(base64.b64decode(encoded, validate=True), limit + 1)
    if (not decoder.eof or decoder.unused_data or decoder.unconsumed_tail or
        not payload or len(payload) > limit):
        raise ValueError("snapshot payload is malformed or too large")
    return payload


def engine_digest(repository: Path) -> str:
    names = subprocess.check_output(
        ["git", "ls-files", "-z", "lib", "include", "cmake", "tools/neverd", "CMakeLists.txt"],
        cwd=repository).decode().split("\0")
    rows = [(name, pe.digest(repository / name)) for name in sorted(filter(None, names))]
    modules = subprocess.check_output(
        ["git", "ls-tree", "HEAD", "third_party/llvm-project", "third_party/capstone", "third_party/unicorn"],
        cwd=repository).decode()
    return hashlib.sha256(json.dumps([rows, modules], separators=(",", ":")).encode()).hexdigest()


def apply_delta(original: bytes, delta: dict) -> bytes:
    if hashlib.sha256(original).hexdigest() != delta["original_sha256"]:
        raise ValueError("snapshot original digest mismatch")
    size = delta["size"]
    if not isinstance(size, int) or size < len(original) or size > 64 * 1024 * 1024:
        raise ValueError("snapshot image size mismatch")
    data = bytearray(original)
    data.extend(b"\0" * (size - len(data)))
    end = 0
    for row in delta["writes"]:
        offset = row["offset"]
        payload = decode_bounded(row["data"], size)
        if (not isinstance(offset, int) or offset < end or offset + len(payload) > size):
            raise ValueError("snapshot write is malformed, overlapping or out of bounds")
        data[offset:offset + len(payload)] = payload
        end = offset + len(payload)
    if hashlib.sha256(data).hexdigest() != delta["sha256"]:
        raise ValueError("snapshot final image digest mismatch")
    return bytes(data)


def prepare(repository: Path, fixtures: Path, output: Path, system: str, arch: str,
            *, profile: RuntimeProfile | None = None,
            producer_commit: str | None = hosted.PRODUCER) -> None:
    manifest = json.loads((fixtures / "manifest.json").read_text(encoding="utf-8"))
    if (manifest.get("schema") != SCHEMA or
        not isinstance(manifest.get("producer_commit"), str) or
        not manifest["producer_commit"] or
        (producer_commit is not None and manifest["producer_commit"] != producer_commit)):
        raise ValueError("unqualified native snapshot")
    if manifest.get("engine_source_digest") != engine_digest(repository):
        raise ValueError("engine source changed; regenerate the native snapshot")
    for name, digest in manifest["source_sha256"].items():
        path = (fixtures / name).resolve()
        if not path.is_relative_to(fixtures) or pe.digest(path) != digest:
            raise ValueError("snapshot source path/digest mismatch")
    for name, digest in manifest.get("producer_inputs", {}).items():
        path = (repository / name).resolve()
        if not path.is_relative_to(repository) or pe.digest(path) != digest:
            raise ValueError("snapshot producer input path/digest mismatch")
    if profile is None:
        if system == "windows":
            profile = RuntimeProfile(hosted.SCHEMA, hosted.CASES, ".exe", pe.machine,
                                     pe.PROFILES[arch][1], frozenset({"c-original"}))
        elif system == "linux":
            profile = RuntimeProfile(elf.SCHEMA, elf.CASES, ".elf", elf.machine,
                                     elf.PROFILES[arch][1],
                                     frozenset({"original-eh", "original-callback"}))
        else:
            raise ValueError("unknown runtime snapshot system")
    rows = [row for row in manifest["images"] if row["system"] == system and row["architecture"] == arch]
    cases = profile.cases
    if len(rows) != len(cases) or {row["case"] for row in rows} != set(cases):
        raise ValueError("snapshot case inventory mismatch")
    output.mkdir(parents=True, exist_ok=False)
    inventory = {"schema": profile.schema,
                 "commit": os.environ.get("GITHUB_SHA", manifest["engine_source_commit"]),
                 "producer_commit": manifest["producer_commit"],
                 "engine_source_commit": manifest["engine_source_commit"],
                 "engine_source_digest": manifest["engine_source_digest"], "images": []}
    for row in rows:
        original_root = fixtures if manifest.get("original_paths") == "fixture" else repository
        base = (original_root / row["original"]).resolve()
        delta_path = (fixtures / row["delta"]).resolve()
        if (not base.is_relative_to(original_root) or not delta_path.is_relative_to(fixtures) or
            pe.digest(delta_path) != row["delta_sha256"]):
            raise ValueError("snapshot input path/digest mismatch")
        delta = json.loads(delta_path.read_text(encoding="utf-8"))
        data = apply_delta(base.read_bytes(), delta)
        if row["case"] not in profile.originals and not row["receipts"]:
            raise ValueError("snapshot replacement has no publication receipt")
        target = output / (row["case"] + profile.suffix)
        target.write_bytes(data)
        if profile.machine(target) != profile.machine_id:
            raise ValueError("snapshot machine mismatch")
        inventory["images"].append({"architecture": arch, "case": row["case"],
                                   "path": target.name, "sha256": pe.digest(target)})
        for index, item in enumerate(row["receipts"]):
            receipt_data = decode_bounded(item["data"], 64 * 1024 * 1024)
            if hashlib.sha256(receipt_data).hexdigest() != item["sha256"]:
                raise ValueError("snapshot publication receipt digest mismatch")
            receipt = json.loads(receipt_data)
            if (receipt.get("schema") != "neverd.source-patch.v1" or
                receipt.get("success") is not True or
                (receipt.get("compiler") == 1 and receipt.get("language") != 0)):
                raise ValueError("snapshot publication receipt is not successful")
            (output / f"{row['case']}-{index}-receipt.json").write_bytes(receipt_data)
    (output / "manifest.json").write_text(json.dumps(inventory, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--system", choices=("windows", "linux"), required=True)
    parser.add_argument("--arch", choices=("x64", "arm64"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repository = Path(__file__).resolve().parents[1]
    fixtures = repository / "unittests/Inputs/source-patch/native-processes"
    prepare(repository, fixtures, args.output.resolve(), args.system, args.arch)
    check = argparse.Namespace(root=args.output, arch=args.arch, runner=None,
                               expected_native_host=args.arch)
    (hosted.execute if args.system == "windows" else elf.execute)(check)


if __name__ == "__main__":
    main()
