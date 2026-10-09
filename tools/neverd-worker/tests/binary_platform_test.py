#!/usr/bin/env python3
"""A binary file decompiles under the conventions its code was built for.

Builds the same C functions for x86-64 Linux and x86-64 Windows with clang
and lld, and opens each program's code alone, as a binary file at the
address it was linked for.  Detection must read System V from the Linux code
and Windows from the Windows code, and a two-argument function must
decompile with two arguments under either -- not with the four a System V
reading of Windows code invents, as IDA's default does.  A platform the user
names is kept as theirs.  Exits 77 (skip) without clang and lld that build
both programs.
"""
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from transport_test import Client

SOURCE = r"""
volatile long first = 1, second = 2;
long sink;

__attribute__((noinline)) long add(long a, long b) { return a + b; }
__attribute__((noinline)) long five(long a, long b, long c, long d, long e) {
  return a * b + c * d - e;
}
__attribute__((noinline)) void keep(long v) { sink = v; }

long entry(void) {
  keep(add(first, second));
  keep(five(first, second, first, second, first));
  keep(add(second, first));
  return sink;
}
"""

TARGETS = {
    "sysv": ["--target=x86_64-linux-gnu", "-fno-pic"],
    "windows": ["--target=x86_64-pc-windows-msvc"],
}


def elf_text(path):
    """The .text address and bytes of an ELF64 file, and its symbols."""
    data = path.read_bytes()
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    sections = []
    for index in range(shnum):
        name, kind, _, addr, offset, size, link = struct.unpack_from(
            "<IIQQQQI", data, shoff + index * shentsize)
        sections.append((name, kind, addr, offset, size, link))
    names = sections[shstrndx]

    def section_name(entry):
        start = names[3] + entry[0]
        return data[start:data.index(b"\0", start)].decode()

    text = next(entry for entry in sections if section_name(entry) == ".text")
    symtab = next(entry for entry in sections if entry[1] == 2)
    strtab = sections[symtab[5]]
    symbols = {}
    for at in range(symtab[3], symtab[3] + symtab[4], 24):
        name, _, _, _, value, _ = struct.unpack_from("<IBBHQQ", data, at)
        start = strtab[3] + name
        symbols[data[start:data.index(b"\0", start)].decode()] = value
    return text[2], data[text[3]:text[3] + text[4]], symbols


def pe_text(path):
    """The .text address and bytes of a PE32+ file, and its COFF symbols."""
    data = path.read_bytes()
    pe, = struct.unpack_from("<I", data, 0x3C)
    sections, = struct.unpack_from("<H", data, pe + 6)
    symbol_table, symbol_count = struct.unpack_from("<II", data, pe + 12)
    optional_size, = struct.unpack_from("<H", data, pe + 20)
    image_base, = struct.unpack_from("<Q", data, pe + 24 + 24)
    table = pe + 24 + optional_size
    addresses = []
    text = None
    for index in range(sections):
        at = table + index * 40
        name = data[at:at + 8].rstrip(b"\0").decode()
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", data, at + 8)
        addresses.append(virtual_address)
        if name == ".text":
            text = (image_base + virtual_address,
                    data[raw_offset:raw_offset + min(virtual_size, raw_size)])
    strings = symbol_table + symbol_count * 18
    symbols = {}
    index = 0
    while index < symbol_count:
        at = symbol_table + index * 18
        short, value, section, _, _, aux = struct.unpack_from("<8sIhHBB", data, at)
        if short[:4] == b"\0\0\0\0":
            offset, = struct.unpack_from("<I", short, 4)
            name = data[strings + offset:data.index(b"\0", strings + offset)].decode()
        else:
            name = short.rstrip(b"\0").decode()
        if section > 0:
            symbols[name] = image_base + addresses[section - 1] + value
        index += 1 + aux
    return text[0], text[1], symbols


def build(directory, platform):
    clang = shutil.which("clang")
    source = Path(directory) / "functions.c"
    source.write_text(SOURCE, encoding="ascii")
    obj = Path(directory) / f"{platform}.o"
    flags = ["-O1", "-ffreestanding", "-fno-stack-protector", "-fno-asynchronous-unwind-tables"]
    compiled = subprocess.run([clang, *TARGETS[platform], *flags, "-c", str(source), "-o", str(obj)],
                              capture_output=True, text=True)
    if compiled.returncode:
        return None
    if platform == "windows":
        image = Path(directory) / "windows.exe"
        linked = subprocess.run([shutil.which("lld-link"), "/entry:entry", "/subsystem:console",
                                 "/nodefaultlib", "/debug:symtab", f"/out:{image}", str(obj)],
                                capture_output=True, text=True)
        return None if linked.returncode else pe_text(image)
    image = Path(directory) / "sysv.elf"
    linked = subprocess.run([shutil.which("ld.lld"), "-e", "entry", "-static", "-o", str(image), str(obj)],
                            capture_output=True, text=True)
    return None if linked.returncode else elf_text(image)


def ok(client, operation, payload=None):
    reply = client.call(operation, payload)
    assert reply["status"] == "ok", reply
    return reply["payload"]


def parameters(text):
    """The parameter count of the first function signature in \\p text."""
    signature = next(line for line in text.splitlines() if re.search(r"\w\s*\(", line))
    inside = signature[signature.index("(") + 1:signature.rindex(")")].strip()
    return 0 if inside in ("", "void") else inside.count(",") + 1


def run(executable):
    if not all(shutil.which(tool) for tool in ("clang", "ld.lld", "lld-link")):
        print("SKIP: needs clang, ld.lld and lld-link")
        return 77
    with tempfile.TemporaryDirectory(prefix="neverd-binary-platform-") as directory:
        for platform in ("sysv", "windows"):
            built = build(directory, platform)
            if built is None:
                print(f"SKIP: clang and lld cannot build the {platform} program")
                return 77
            base, code, symbols = built
            blob = Path(directory) / f"{platform}.bin"
            blob.write_bytes(code)
            client = Client(executable)
            try:
                opened = ok(client, "open", {"path": str(blob), "loader": "binary",
                                             "processor": "x86_64", "base": hex(base),
                                             "entry": hex(symbols["entry"])})
                load = opened["load_options"]
                assert (load["platform"], load["platform_source"]) == (platform, "detected"), load
                text = ok(client, "decompile", {"address": hex(symbols["add"])})["text"]
                assert parameters(text) == 2, (platform, text)
                text = ok(client, "decompile", {"address": hex(symbols["five"])})["text"]
                assert parameters(text) == 5, (platform, text)
            finally:
                client.close()
            # The user's platform is theirs, and the file reopens under it.
            client = Client(executable)
            try:
                chosen = "darwin" if platform == "sysv" else "sysv"
                opened = ok(client, "open", {"path": str(blob), "loader": "binary",
                                             "processor": "x86_64", "base": hex(base),
                                             "platform": chosen})
                assert (opened["load_options"]["platform"],
                        opened["load_options"]["platform_source"]) == (chosen, "user"), opened
            finally:
                client.close()
            client = Client(executable)
            try:
                reopened = ok(client, "open", {"path": str(blob)})
                assert reopened["load_options"]["platform"] == chosen, reopened
            finally:
                client.close()
    print("binary platform: System V and Windows code read as built, arguments as declared")
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1]))
