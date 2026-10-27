"""Load the captured MSVC finally probe without running its EXE startup."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import struct

if __package__:
    from .check_windows_registration_rewrite import PE32
    from .windows_registration_image import code_owner, jump_target
else:
    from check_windows_registration_rewrite import PE32
    from windows_registration_image import code_owner, jump_target

EXPORT = b"nonlocal_finally_return"
OBSERVATION = re.compile(r"(\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)\s*")
GOTO_OBSERVATION = re.compile(
    r"(\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)\s*")


def captured_input(root: Path, source: Path, profile: str) -> Path:
    manifest = json.loads((root / "x86/nonlocal-finally.json").read_text())
    if manifest.get("schema") != 2 or manifest.get("passed") is not True or \
            manifest.get("evidence") != "original-msvc-nonlocal-finally" or \
            manifest.get("architecture") != "x86" or not manifest.get("toolset") or \
            manifest.get("source_sha256") != hashlib.sha256(source.read_bytes()).hexdigest():
        raise ValueError("native MSVC input has no matching source observation")
    cases = [case for case in manifest.get("cases", []) if case.get("case") == profile]
    image = root / "x86" / profile / "original.exe"
    if len(cases) != 1 or cases[0].get("passed") is not True or \
            cases[0].get("negative_control") is not profile.endswith("-control") or \
            cases[0].get("sha256") != hashlib.sha256(image.read_bytes()).hexdigest():
        raise ValueError("native MSVC input bytes changed after observation")
    return image


def as_library(data: bytes, export: bytes = EXPORT) -> bytes:
    """Change only loader headers; preserve all source code and language tables.

    This fixture's exported function needs imports but no EXE startup. TLS
    callbacks would invalidate that assumption and are explicitly rejected.
    Disable ASLR so the observer can require each chosen relocation base.
    """
    image = PE32(data)
    if image.directory(9) != (0, 0) or image.u16(image.optional - 2) & 0x2000:
        raise ValueError("finally observer requires a TLS-free EXE input")
    entry = image.entry(export)
    code_owner(image, entry, entry + 1, ".text")
    result = bytearray(data)
    struct.pack_into("<H", result, image.optional - 2,
                     image.u16(image.optional - 2) | 0x2000)
    struct.pack_into("<I", result, image.optional + 16, 0)
    struct.pack_into("<H", result, image.optional + 70,
                     image.u16(image.optional + 70) & ~0x40)
    struct.pack_into("<I", result, image.optional + 64, 0)
    return bytes(result)


def generated_entry(original: bytes, product: bytes, export: bytes = EXPORT) -> int:
    before, after = PE32(original), PE32(product)
    entry = before.entry(export)
    if before.base != after.base or after.entry(export) != entry or original == product:
        raise ValueError("finally replacement changed its original entry identity")
    target = jump_target(after, entry)
    code_owner(after, target, target + 1, ".ndtext")
    return target


def observe(result: dict, base: int, control: bool = False,
            form: str = "return") -> tuple[int, ...]:
    if form not in ("return", "goto"):
        raise ValueError("unknown native finally observer form")
    pattern = GOTO_OBSERVATION if form == "goto" else OBSERVATION
    match = pattern.fullmatch(result.get("stdout", ""))
    if not match or result.get("exit_code") != int(control):
        raise ValueError("native finally observer did not complete")
    values = tuple(map(int, match.groups()))
    expected = (128, 18, 0, 1, 1, 0, 64, base) if control else \
               (123, 13, 0, 1, 1, 1, 64, base)
    if form == "goto":
        expected = (128457, 12857, 18567, 1857, 0, 1, 1, 0, 128, base) if control else \
                   (123457, 12357, 13567, 1357, 0, 1, 1, 1, 128, base)
    if values != expected:
        raise ValueError("finally return, exception chain, guards or load base differs")
    return values
