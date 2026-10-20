#!/usr/bin/env python3
"""Independent DEX declarations for real guest JNI invocation tests."""
import hashlib
import struct
import zlib

from benchmark_mobile_inventory import uleb128


def native_calls_dex():
    base, child, bad = [f"Lnativecall/{n};" for n in ("Base", "Child", "Bad")]
    obj, string = "Ljava/lang/Object;", "Ljava/lang/String;"
    # owner, name, return, parameters, flags, optional code words
    declarations = [
        (base, "number", "I", ["I"], 0x109, None),
        (base, "echo", obj, [obj], 0x109, None),
        (base, "self", obj, [], 0x101, None),
        (base, "wide", "J", ["J"], 0x101, None),
        (child, "wide", "J", ["J"], 0x101, None),
        (base, "mixed", "J", ["Z", "B", "C", "S", "I", "J"], 0x109, None),
        (base, "visit", "V", [], 0x109, None),
        (base, "text", string, [string], 0x109, None),
        (base, "booleanValue", "Z", [], 0x109, None),
        (base, "byteValue", "B", [], 0x109, None),
        (base, "shortValue", "S", [], 0x109, None),
        (base, "charValue", "C", [], 0x109, None),
        (base, "locked", "V", [], 0x129, None),
        (base, "overflow", "I", ["I"] * 7, 0x109, None),
        (base, "floating", "F", ["F"], 0x109, None),
        (base, "missing", "I", [], 0x109, None),
        (base, "ordinary", "I", [], 9, [0x0013, 99, 0x000F]),
        (bad, "<clinit>", "V", [], 0x10008, [0x0012, 0x0027]),
        (bad, "after", "V", [], 0x109, None),
    ]
    parents = {base: obj, child: base, bad: obj}
    types = sorted({obj, string, *parents, *[d[2] for d in declarations],
                    *[t for d in declarations for t in d[3]]})
    ti = {t: i for i, t in enumerate(types)}
    protos = sorted({(d[2], tuple(d[3])) for d in declarations},
                    key=lambda p: (ti[p[0]], tuple(ti[t] for t in p[1])))

    def shorty(proto):
        return "".join("L" if t.startswith(("L", "[")) else t
                       for t in (proto[0], *proto[1]))

    strings = sorted({*types, *[d[1] for d in declarations],
                      *[shorty(p) for p in protos]})
    si = {s: i for i, s in enumerate(strings)}
    methods = sorted(declarations,
                     key=lambda d: (ti[d[0]], si[d[1]],
                                    protos.index((d[2], tuple(d[3])))))
    data = bytearray(112)
    sections = [(0, 1, 0)]

    def table(kind, count, size):
        at = len(data)
        data.extend(bytes(count * size))
        sections.append((kind, count, at))
        return at

    so = table(1, len(strings), 4)
    to = table(2, len(types), 4)
    po = table(3, len(protos), 12)
    mo = table(5, len(methods), 8)
    co = table(6, len(parents), 32)
    data_start = len(data)
    sections.append((0x2002, len(strings), len(data)))
    for i, s in enumerate(strings):
        struct.pack_into("<I", data, so + 4*i, len(data))
        data.extend(uleb128(len(s)) + s.encode() + b"\0")
    for i, t in enumerate(types):
        struct.pack_into("<I", data, to + 4*i, si[t])
    lists = {}
    for _, args in protos:
        if args and args not in lists:
            data.extend(bytes(-len(data) % 4))
            lists[args] = len(data)
            data.extend(struct.pack("<I", len(args)))
            data.extend(struct.pack("<" + "H"*len(args), *[ti[t] for t in args]))
    sections.append((0x1001, len(lists), min(lists.values())))
    for i, p in enumerate(protos):
        struct.pack_into("<III", data, po + 12*i, si[shorty(p)], ti[p[0]],
                         lists.get(p[1], 0))
    code = {}
    for i, (owner, name, ret, args, flags, words) in enumerate(methods):
        struct.pack_into("<HHI", data, mo + 8*i, ti[owner],
                         protos.index((ret, tuple(args))), si[name])
        if words:
            data.extend(bytes(-len(data) % 4))
            code[i] = len(data)
            data.extend(struct.pack("<HHHHII", 1, 0, 0, 0, 0, len(words)))
            data.extend(struct.pack("<" + "H"*len(words), *words))
    sections.append((0x2001, len(code), min(code.values())))
    offsets = {}
    for owner in sorted(parents):
        offsets[owner] = len(data)
        direct = [i for i, m in enumerate(methods) if m[0] == owner and m[4] & 8]
        virtual = [i for i, m in enumerate(methods) if m[0] == owner and not m[4] & 8]
        data.extend(b"\0\0" + uleb128(len(direct)) + uleb128(len(virtual)))
        for group in (direct, virtual):
            last = 0
            for i in group:
                data.extend(uleb128(i-last) + uleb128(methods[i][4]) +
                            uleb128(code.get(i, 0)))
                last = i
    sections.append((0x2000, len(offsets), min(offsets.values())))
    for i, owner in enumerate(sorted(parents)):
        struct.pack_into("<8I", data, co + 32*i, ti[owner], 1, ti[parents[owner]],
                         0, 0xFFFFFFFF, 0, offsets[owner], 0)
    data.extend(bytes(-len(data) % 4))
    map_at = len(data)
    sections.append((0x1000, 1, map_at))
    data.extend(struct.pack("<I", len(sections)))
    for kind, count, at in sorted(sections, key=lambda s: s[2]):
        data.extend(struct.pack("<HHII", kind, 0, count, at))
    data[:8] = b"dex\n035\0"
    struct.pack_into("<20I", data, 32, len(data), 112, 0x12345678, 0, 0, map_at,
                     len(strings), so, len(types), to, len(protos), po, 0, 0,
                     len(methods), mo, len(parents), co,
                     len(data)-data_start, data_start)
    data[12:32] = hashlib.sha1(data[32:]).digest()
    struct.pack_into("<I", data, 8, zlib.adler32(data[12:]) & 0xFFFFFFFF)
    return data
