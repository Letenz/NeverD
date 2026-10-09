"""A small valid PE for filesystem-boundary tests; never executed."""
import struct


def image():
    wide = True
    data = bytearray(0x800)
    optional, optional_size = 0x98, 240 if wide else 224
    directories = optional + (112 if wide else 96)
    base, width = (0x140000000, 8) if wide else (0x774c0000, 4)
    def u16(at, value): struct.pack_into("<H", data, at, value)
    def u32(at, value): struct.pack_into("<I", data, at, value)
    def word(at, value): struct.pack_into("<Q" if wide else "<I", data, at, value)
    data[:2] = b"MZ"
    u32(0x3c, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    u16(0x84, 0x8664 if wide else 0x14c)
    u16(0x86, 3)
    u16(0x94, optional_size)
    u16(0x96, 2)
    u16(optional, 0x20b if wide else 0x10b)
    u32(optional + 16, 0x1000)
    word(optional + (24 if wide else 28), base)
    u32(optional + 32, 0x1000)
    u32(optional + 36, 0x200)
    u32(optional + 56, 0x4000)
    u32(optional + 60, 0x200)
    u16(optional + 68, 3)
    u32(directories - 4, 16)
    for index, name in enumerate((b".text", b".idata", b".reloc")):
        section = optional + optional_size + index * 40
        data[section:section + len(name)] = name
        u32(section + 8, 0x200)
        u32(section + 12, 0x1000 * (index + 1))
        u32(section + 16, 0x200)
        u32(section + 20, 0x200 * (index + 1))
        u32(section + 36, 0x60000020 if not index else 0xc0000040)
    code = (b"\x48\xb8" + struct.pack("<Q", 42) + b"\xc3"
            if wide else b"\xb8\x2a\0\0\0\xc3")
    data[0x200:0x200 + len(code)] = code
    u32(directories + 8, 0x2000)
    u32(directories + 12, 40)
    u32(0x400, 0x2100)
    u32(0x40c, 0x2080)
    u32(0x410, 0x2180)
    data[0x480:0x48c] = b"example.dll\0"
    word(0x500, (1 << (width * 8 - 1)) | 1)
    word(0x580, (1 << (width * 8 - 1)) | 1)
    return bytes(data), base
