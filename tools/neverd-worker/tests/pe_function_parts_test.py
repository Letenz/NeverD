#!/usr/bin/env python3
"""Real-engine x64 PE functions as .pdata describes them; the fixture is never
executed.

The first function's record covers the int3 its compiler pads it with after
its return; the second jumps to a cold part far below it, which a record
chained to its own covers.  The entry point is a third, which analysis sizes
by its code rather than by its record.
"""
from pathlib import Path
import struct
import sys
import tempfile
from transport_test import Client

BASE = 0x140000000
PADDED, SPLIT, ENTRY, COLD = 0x1000, 0x1010, 0x1020, 0x1100
UNW_FLAG_CHAININFO = 4


def image():
    data = bytearray(0x800)
    optional, optional_size = 0x98, 240
    directories = optional + 112
    def u16(at, value): struct.pack_into("<H", data, at, value)
    def u32(at, value): struct.pack_into("<I", data, at, value)
    data[:2] = b"MZ"
    u32(0x3c, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    u16(0x84, 0x8664)
    u16(0x86, 3)
    u16(0x94, optional_size)
    u16(0x96, 0x22)
    u16(optional, 0x20b)
    u32(optional + 16, ENTRY)
    struct.pack_into("<Q", data, optional + 24, BASE)
    u32(optional + 32, 0x1000)
    u32(optional + 36, 0x200)
    u32(optional + 56, 0x4000)
    u32(optional + 60, 0x200)
    u16(optional + 68, 3)
    u32(directories - 4, 16)
    # Code, unwind information, and the RUNTIME_FUNCTIONs.
    sections = ((b".text", 0x60000020), (b".rdata", 0x40000040), (b".pdata", 0x40000040))
    for index, (name, flags) in enumerate(sections):
        section = optional + optional_size + index * 40
        data[section:section + len(name)] = name
        u32(section + 8, 0x200)
        u32(section + 12, 0x1000 * (index + 1))
        u32(section + 16, 0x200)
        u32(section + 20, 0x200 * (index + 1))
        u32(section + 36, flags)
    text = 0x200
    data[text:text + 0x200] = b"\xcc" * 0x200
    def code(rva, encoded): data[text + rva - 0x1000:text + rva - 0x1000 + len(encoded)] = encoded
    code(PADDED, b"\x48\xc7\xc0\x2a\x00\x00\x00\xc3\xcc")  # mov rax, 42; ret; int3
    code(SPLIT, b"\xe9" + struct.pack("<i", COLD - (SPLIT + 5)))  # jmp cold
    code(ENTRY, b"\xc3")  # ret
    code(COLD, b"\x31\xc0\xc3")  # xor eax, eax; ret
    rdata = 0x400
    data[rdata:rdata + 4] = bytes([1, 0, 0, 0])
    data[rdata + 0x10:rdata + 0x14] = bytes([1, 0, 0, 0])
    data[rdata + 0x20:rdata + 0x24] = bytes([1 | UNW_FLAG_CHAININFO << 3, 0, 0, 0])
    struct.pack_into("<III", data, rdata + 0x24, SPLIT, SPLIT + 5, 0x2010)
    data[rdata + 0x30:rdata + 0x34] = bytes([1, 0, 0, 0])
    records = ((PADDED, PADDED + 9, 0x2000), (SPLIT, SPLIT + 5, 0x2010),
               (ENTRY, ENTRY + 1, 0x2030), (COLD, COLD + 3, 0x2020))
    for index, record in enumerate(records):
        struct.pack_into("<III", data, 0x600 + 12 * index, *record)
    u32(directories + 3 * 8, 0x3000)
    u32(directories + 3 * 8 + 4, 12 * len(records))
    return bytes(data)


def lines(client, address, after):
    listed = client.call("listing", {"address": hex(BASE + address), "before": 0, "after": after})
    assert listed["status"] == "ok", listed
    return listed["payload"]["lines"]


def run(worker):
    with tempfile.TemporaryDirectory(prefix="neverd-pe-parts-") as directory:
        path = Path(directory) / "parts.exe"
        path.write_bytes(image())
        with Client(worker) as client:
            assert client.call("open", {"path": str(path)})["status"] == "ok"
            listed = client.call("functions", {"limit": 10})["payload"]["items"]
            entries = {int(item["address"], 16) for item in listed}
            assert {BASE + PADDED, BASE + SPLIT} <= entries, listed
            assert BASE + COLD not in entries, listed

            # The padded function ends at its return; its int3 is padding.
            padded = [" ".join(line["text"].split()) for line in lines(client, PADDED, 40)]
            footer = next(i for i, text in enumerate(padded) if text.endswith(" endp"))
            assert padded[footer - 1] == "retn", padded
            assert not any(text == "int3" for text in padded[:footer]), padded

            # The jump reaches the cold part by a label, and the part lists
            # as a chunk of its function.
            split = [" ".join(line["text"].split()) for line in lines(client, SPLIT, 12)]
            assert "jmp loc_140001100" in split, split
            cold = lines(client, COLD, 16)
            texts = [" ".join(line["text"].split()) for line in cold]
            assert "; START OF FUNCTION CHUNK FOR sub_140001010" in texts, texts
            assert "; END OF FUNCTION CHUNK FOR sub_140001010" in texts, texts
            assert any(text.startswith("loc_140001100:") for text in texts), texts
            body = [line for line in cold if line["kind"] == "insn"
                    and BASE + COLD <= int(line["address"], 16) < BASE + COLD + 3]
            assert len(body) == 2 and all(line["function_address"] == hex(BASE + SPLIT) for line in body), body
    print("x64 PE padding and chained chunks list as IDA lists them")


if __name__ == "__main__":
    run(sys.argv[1])
