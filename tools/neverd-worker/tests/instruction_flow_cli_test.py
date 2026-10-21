#!/usr/bin/env python3
"""CLI and worker reference pages agree on synthetic instruction sweeps."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from transport_test import Client

ENTRY = 0x400078


def fixture(count):
    # Explicit symbol extents avoid depending on a detector heuristic. Both
    # 8-byte and 4-byte loads are followed by NOP/RET: scratch reuse must not
    # give those trailing instructions a preceding instruction's reference.
    target = ENTRY + 16 * count
    code = bytearray()
    expected = []
    names = bytearray(b"\0")
    symbols = bytearray(24)
    for index in range(count):
        start = ENTRY + 16 * index
        load = bytes.fromhex("488b05" if index % 2 == 0 else "8b05")
        body = load + struct.pack("<i", target - start - len(load) - 4) + bytes.fromhex("90c3")
        code += body + b"\xcc" * (16 - len(body))
        symbols += struct.pack("<IBBHQQ", len(names), 0x12, 0, 1, start, len(body))
        names += f"fixture_{index}".encode() + b"\0"
        expected.append((start, target, "read", 8 if index % 2 == 0 else 4))
    data = b"fixture text\0"
    image = bytearray(120) + code + data

    def append(contents, alignment=1):
        image.extend(bytes((-len(image)) % alignment))
        offset = len(image)
        image.extend(contents)
        return offset

    symbol_offset = append(symbols, 8)
    name_offset = append(names)
    section_names = b"\0.text\0.rodata\0.symtab\0.strtab\0.shstrtab\0"
    section_name_offset = append(section_names)
    sections = bytes(64)
    for name, kind, flags, address, offset, size, link, info, alignment, entry_size in [
        (1, 1, 6, ENTRY, 120, len(code), 0, 0, 16, 0),
        (7, 1, 2, target, 120 + len(code), len(data), 0, 0, 1, 0),
        (15, 2, 0, 0, symbol_offset, len(symbols), 4, 1, 8, 24),
        (23, 3, 0, 0, name_offset, len(names), 0, 0, 1, 0),
        (31, 3, 0, 0, section_name_offset, len(section_names), 0, 0, 1, 0),
    ]:
        sections += struct.pack("<IIQQQQIIQQ", name, kind, flags, address, offset, size,
                                link, info, alignment, entry_size)
    section_offset = append(sections, 8)
    image[:64] = struct.pack("<16sHHIQQQIHHHHHH", b"\x7fELF\x02\x01\x01" + bytes(9),
                             2, 62, 1, ENTRY, 64, section_offset, 0, 64, 56, 1, 64, 6, 5)
    mapped = 120 + len(code) + len(data)
    image[64:120] = struct.pack("<IIQQQQQQ", 1, 5, 0, 0x400000, 0x400000, mapped, mapped, 4096)
    return image, expected


def run(worker, cli):
    with tempfile.TemporaryDirectory(prefix="neverd-flow-cli-") as directory:
        for count in (2, 2049):
            binary = Path(directory) / f"functions-{count}.elf"
            data, expected = fixture(count)
            binary.write_bytes(data)
            target = expected[0][1]
            for _ in range(2):
                result = subprocess.run([cli, "xrefs", str(binary), "--addr", hex(target),
                                         "--no-debug", "--json"], capture_output=True,
                                        text=True, timeout=60)
                assert result.returncode == 0, result.stdout + result.stderr
                rows = json.loads(result.stdout)
                actual = [(int(r["from"], 16), int(r["to"], 16), r["kind"], r.get("size", 0))
                          for r in rows]
                assert actual == expected, (count, len(actual), actual[:4])
            with Client(worker) as client:
                opened = client.call("open", {"path": str(binary), "analysis": False, "debug_info": False})
                assert opened["status"] == "ok", opened
                actual = []
                while len(actual) < count:
                    reply = client.call("xrefs", {"address": hex(target), "offset": len(actual), "limit": 512})
                    assert reply["status"] == "ok", reply
                    page = reply["payload"]
                    assert page["total"] == count, page
                    assert page["items"], page
                    actual.extend((int(r["from"], 16), int(r["to"], 16), r["kind"])
                                  for r in page["items"])
                assert actual == [r[:3] for r in expected]


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("worker")
    parser.add_argument("cli")
    args = parser.parse_args()
    run(args.worker, args.cli)
    print("CLI/worker references, access widths and complete multi-page inventories passed")
