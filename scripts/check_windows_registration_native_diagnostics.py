#!/usr/bin/env python3
"""Diagnose source-fixture runtime failures without changing replay evidence."""
import argparse
import hashlib
import json
import os
import struct
from pathlib import Path
from check_windows_registration_eh import run_image
from check_windows_registration_rewrite import PE32


def initialize_saved_esp(pe: PE32, data: bytearray, entry: int) -> None:
    """Redirect the fixture's first call through an address-independent stub."""
    call = entry + 39
    if data[call] != 0xe8:
        raise ValueError('source fixture no longer has its expected raise call')
    call_rva = pe.entry() + 39
    target = call_rva + 5 + struct.unpack_from('<i', data, call + 1)[0]
    section = next(s for s in pe.sections if s[0] == '.text')
    _, rva, virtual_size, raw, raw_size = section
    stub_rva = rva + virtual_size
    # The stub is entered with an extra return PC. Save the caller's ESP,
    # then tail-call the same source callee; no absolute fixup is introduced.
    stub = bytes.fromhex('8d4424048945e8e9')
    stub += struct.pack('<i', target - (stub_rva + len(stub) + 4))
    if virtual_size + len(stub) > raw_size:
        raise ValueError('diagnostic stub does not fit the fixture text padding')
    data[raw + virtual_size:raw + virtual_size + len(stub)] = stub
    struct.pack_into('<i', data, call + 1, stub_rva - call_rva - 5)
    table = pe.optional + pe.u16(pe.optional - 4)
    index = pe.sections.index(section)
    struct.pack_into('<I', data, table + index * 40 + 8,
                     virtual_size + len(stub))


def native_runtime_bytes() -> dict | None:
    if os.name != 'nt':
        return None
    path = Path(os.environ['WINDIR']) / 'SysWOW64/msvcrt.dll'
    runtime = PE32(path.read_bytes())
    rva = runtime.entry(b'_except_handler3')
    raw = runtime.raw(rva, 2048)
    return {'path': str(path), 'sha256': hashlib.sha256(runtime.data).hexdigest(),
            'image_base': runtime.base, 'except_handler3_rva': rva,
            'except_handler3_bytes': runtime.data[raw:raw + 2048].hex()}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--wine')
    args = parser.parse_args()
    launcher = [args.wine] if args.wine else []
    if os.name != 'nt' and not launcher:
        parser.error('native Windows is required unless --wine is explicit')
    pe = PE32((args.root / 'evidence/source-filter/original.exe').read_bytes())
    entry = pe.raw(pe.entry())
    if pe.data[entry:entry + 5] != bytes.fromhex('5589e56aff'):
        raise ValueError('source fixture is not the expected SEH3 frame')
    table = pe.u32(entry + 6) - pe.base
    filter_rva = pe.u32(pe.raw(table) + 4) - pe.base
    handler_rva = pe.u32(pe.raw(table) + 8) - pe.base
    config_rva, _ = pe.directory(10)
    config = pe.raw(config_rva)
    observations = []
    variants = ('original', 'constant-filter', 'no-safe-seh', 'no-load-config',
                'saved-esp', 'filter-breakpoint', 'handler-breakpoint')
    for variant in variants:
        data = bytearray(pe.data)
        if variant == 'constant-filter':
            start = pe.raw(filter_rva, 6)
            data[start:start + 6] = bytes.fromhex('b801000000c3')
        elif variant == 'no-safe-seh':
            struct.pack_into('<2I', data, config + 64, 0, 0)
        elif variant == 'no-load-config':
            struct.pack_into('<2I', data, pe.optional + 96 + 10 * 8, 0, 0)
        elif variant == 'saved-esp':
            initialize_saved_esp(pe, data, entry)
        elif variant == 'filter-breakpoint':
            data[pe.raw(filter_rva, 1)] = 0xcc
        elif variant == 'handler-breakpoint':
            data[pe.raw(handler_rva, 1)] = 0xcc
        image = args.root / ('diagnostic-' + variant + '.exe')
        image.write_bytes(data)
        result = run_image(image, launcher, os.environ.copy(), 60)
        observations.append({'variant': variant, 'runtime': result})
    (args.root / 'runtime-diagnostics.json').write_text(json.dumps({
        'evidence': 'diagnostic-variants-only',
        'source_sha256': hashlib.sha256(pe.data).hexdigest(),
        'native_runtime': native_runtime_bytes(),
        'observations': observations}, indent=2))
    print(json.dumps(observations, indent=2))


if __name__ == '__main__':
    main()
