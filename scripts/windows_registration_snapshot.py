"""Derive a SavedESP writeback probe from the checked MSVC C++ fixtures.

The RTTI, throw, cleanup, handler and main harness remain compiler generated.
The bounded edits deliberately make the catch overwrite SavedESP with 7 and
make its continuation read the initialized inner guard through SavedESP.
Both the runtime's physical restore and NeverD's recovered-frame writeback
are therefore observable.
This is a derived machine-code probe, not a new MSVC compilation receipt.
"""

from __future__ import annotations

import struct

if __package__:
    from .check_windows_registration_rewrite import PE32
else:
    from check_windows_registration_rewrite import PE32


def saved_stack_probe(image: PE32, reference: bool) -> bytes:
    entry = image.entry(b"registration_cxx_probe")
    data = bytearray(image.data)

    def expect(rva: int, expected: bytes) -> int:
        offset = image.raw(rva, len(expected))
        if data[offset:offset + len(expected)] != expected:
            raise ValueError("SavedESP probe requires the checked MSVC fixture shape")
        return offset

    expect(entry, bytes.fromhex("55 8b ec 6a ff 68"))
    expect(entry + 0x18, bytes.fromhex("51 83 ec 10 53 56 57 89 65 f0"))
    # The throw cannot return. Reuse only the old normal cleanup path for the
    # resumed observation, retaining the independently table-owned cleanups.
    dead = entry + 0x45
    inner, outer = ((0xe8, 0xe4) if reference else (0xec, 0xe8))
    expected = (bytes.fromhex("c6 45 fc 01 8d 4d") + bytes([inner]) +
                bytes.fromhex("e8 5f ff ff ff c6 45 fc 00 8d 4d") +
                bytes([outer]) + bytes.fromhex("e8 53 ff ff ff 90"))
    region = expect(dead, expected)
    store_rva = entry + (0x8f if reference else 0x7b)
    store = expect(store_rva, bytes.fromhex("89 4d e0" if reference else "89 55 e0"))
    resume = store_rva + 3
    original_target = entry + (0xa7 if reference else 0x93)
    pointer = expect(resume, b"\xb8" + struct.pack("<I", image.base + original_target) + b"\xc3")
    epilogue = entry + (0xb8 if reference else 0xa4)
    expect(epilogue, bytes.fromhex("8b 4d f4 64 89 0d 00 00 00 00 5f 5e 5b 8b e5 5d c3"))
    fields = image.relocation_fields()
    if resume + 1 not in fields or any(dead <= field + 3 and field < dead + len(expected)
                                       for field in fields):
        raise ValueError("SavedESP probe lost its exact relocation ownership")
    data[store + 2] = 0xf0
    struct.pack_into("<I", data, pointer + 1, image.base + dead)
    # mov [ebp-4], -1; mov eax, [ebp-16]; mov eax, [eax + guard]; add eax, 5
    # The initialized inner guard remains 2 after its read-only destructor.
    code = (bytes.fromhex("c7 45 fc ff ff ff ff 8b 45 f0 8b 40") +
            bytes([20 if reference else 24]) + bytes.fromhex("83 c0 05 eb"))
    displacement = epilogue - (dead + len(code) + 1)
    if not -128 <= displacement <= 127:
        raise ValueError("SavedESP observation has no bounded epilogue branch")
    code += struct.pack("b", displacement)
    data[region:region + len(expected)] = code + b"\xcc" * (len(expected) - len(code))
    return bytes(data)
