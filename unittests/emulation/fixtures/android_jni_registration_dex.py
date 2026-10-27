#!/usr/bin/env python3
"""Encode authored JNI registration declarations and a throwing initializer.

DEX layout: https://source.android.com/docs/core/runtime/dex-format
Native methods carry no code; Deferred.<clinit> is const/4 v0, 0; throw v0.
"""

# Original declarations for registration/initialization tests. Native targets are
# opaque, and Deferred.<clinit> deliberately throws null on its first active use.
import hashlib
import struct
import zlib
from benchmark_mobile_inventory import uleb128


def registration_dex():
    parent, child, deferred = [
        f"Lregistration/{n};" for n in ("Parent", "Child", "Deferred")
    ]
    declarations = {
        parent: [("alpha", "(I)I", 0x109, None), ("beta", "(J)J", 0x101, None)],
        child: [("child", "()V", 0x109, None), ("ordinary", "()V", 9, [0x000E])],
        deferred: [
            ("<clinit>", "()V", 0x10008, [0x0012, 0x0027]),
            ("after", "()V", 0x109, None),
        ],
    }
    types = sorted([*declarations, "Ljava/lang/Object;", "I", "J", "V"])
    protos = [("I", ["I"]), ("J", ["J"]), ("V", [])]
    strings = sorted(
        set(types + ["II", "JJ"] + [m[0] for ms in declarations.values() for m in ms])
    )
    methods = sorted(
        [(owner, *m) for owner, ms in declarations.items() for m in ms],
        key=lambda m: (
            types.index(m[0]),
            strings.index(m[1]),
            ["(I)I", "(J)J", "()V"].index(m[2]),
        ),
    )
    data = bytearray(112)
    sections = [(0, 1, 0)]

    def table(kind, count, size):
        at = len(data)
        data.extend(bytes(count * size))
        sections.append((kind, count, at))
        return at

    so = table(1, len(strings), 4)
    to = table(2, len(types), 4)
    po = table(3, 3, 12)
    mo = table(5, len(methods), 8)
    co = table(6, 3, 32)
    data_start = len(data)
    sections.append((0x2002, len(strings), len(data)))
    for i, s in enumerate(strings):
        struct.pack_into("<I", data, so + 4 * i, len(data))
        data.extend(uleb128(len(s)) + s.encode() + b"\0")
    for i, t in enumerate(types):
        struct.pack_into("<I", data, to + 4 * i, strings.index(t))
    param_at = []
    for ret, args in protos:
        if args:
            data.extend(bytes(-len(data) % 4))
            param_at.append(len(data))
            data.extend(struct.pack("<IH", 1, types.index(args[0])))
    sections.append((0x1001, 2, param_at[0]))
    for i, (ret, args) in enumerate(protos):
        struct.pack_into(
            "<III",
            data,
            po + 12 * i,
            strings.index(ret + "".join(args)),
            types.index(ret),
            param_at[i] if args else 0,
        )
    code = {}
    for i, (owner, name, sig, flags, words) in enumerate(methods):
        struct.pack_into(
            "<HHI",
            data,
            mo + 8 * i,
            types.index(owner),
            ["(I)I", "(J)J", "()V"].index(sig),
            strings.index(name),
        )
        if words:
            data.extend(bytes(-len(data) % 4))
            code[i] = len(data)
            data.extend(
                struct.pack("<HHHHII", int(name == "<clinit>"), 0, 0, 0, 0, len(words))
            )
            data.extend(struct.pack("<" + "H" * len(words), *words))
    sections.append((0x2001, len(code), min(code.values())))
    offsets = {}
    for owner in sorted(declarations):
        offsets[owner] = len(data)
        direct = [i for i, m in enumerate(methods) if m[0] == owner and m[3] & 8]
        virtual = [i for i, m in enumerate(methods) if m[0] == owner and not m[3] & 8]
        data.extend(b"\0\0" + uleb128(len(direct)) + uleb128(len(virtual)))
        for group in (direct, virtual):
            last = 0
            for i in group:
                data.extend(
                    uleb128(i - last) + uleb128(methods[i][3]) + uleb128(code.get(i, 0))
                )
                last = i
    sections.append((0x2000, 3, min(offsets.values())))
    for i, owner in enumerate(sorted(declarations)):
        struct.pack_into(
            "<8I",
            data,
            co + 32 * i,
            types.index(owner),
            1,
            types.index(parent if owner == child else "Ljava/lang/Object;"),
            0,
            0xFFFFFFFF,
            0,
            offsets[owner],
            0,
        )
    data.extend(bytes(-len(data) % 4))
    map_at = len(data)
    sections.append((0x1000, 1, map_at))
    data.extend(struct.pack("<I", len(sections)))
    for kind, count, at in sorted(sections, key=lambda s: s[2]):
        data.extend(struct.pack("<HHII", kind, 0, count, at))
    data[:8] = b"dex\n035\0"
    struct.pack_into(
        "<20I",
        data,
        32,
        len(data),
        112,
        0x12345678,
        0,
        0,
        map_at,
        len(strings),
        so,
        len(types),
        to,
        3,
        po,
        0,
        0,
        len(methods),
        mo,
        3,
        co,
        len(data) - data_start,
        data_start,
    )
    data[12:32] = hashlib.sha1(data[32:]).digest()
    struct.pack_into("<I", data, 8, zlib.adler32(data[12:]) & 0xFFFFFFFF)
    return data
