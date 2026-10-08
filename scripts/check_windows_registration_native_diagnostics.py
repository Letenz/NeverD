#!/usr/bin/env python3
import argparse, hashlib, json, os, struct
from pathlib import Path
from check_windows_registration_eh import run_image
from check_windows_registration_rewrite import PE32
parser = argparse.ArgumentParser(description="Diagnose original native SEH3 runtime acceptance; variants are not reconstruction evidence.")
parser.add_argument('--root', required=True, type=Path)
parser.add_argument('--wine')
args = parser.parse_args()
source = args.root / 'evidence/source-filter/original.exe'
launcher = [args.wine] if args.wine else []
if os.name != 'nt' and not launcher:
    parser.error('native Windows is required unless --wine is explicit')
pe = PE32(source.read_bytes())
entry = pe.raw(pe.entry())
assert pe.data[entry:entry+5] == bytes.fromhex('5589e56aff')
table = pe.u32(entry + 6) - pe.base
filter_rva = pe.u32(pe.raw(table) + 4) - pe.base
config_rva, _ = pe.directory(10)
config = pe.raw(config_rva)
observations = []
for variant in ('original', 'constant-filter', 'no-safe-seh', 'no-load-config'):
    data = bytearray(pe.data)
    if variant == 'constant-filter':
        start = pe.raw(filter_rva, 6)
        data[start:start+6] = bytes.fromhex('b801000000c3')
    if variant == 'no-safe-seh':
        struct.pack_into('<2I', data, config + 64, 0, 0)
    if variant == 'no-load-config':
        struct.pack_into('<2I', data, pe.optional + 96 + 10*8, 0, 0)
    image = args.root / ('diagnostic-' + variant + '.exe')
    image.write_bytes(data)
    result = run_image(image, launcher, os.environ.copy(), 60)
    observations.append({'variant': variant, 'runtime': result})
(args.root / 'runtime-diagnostics.json').write_text(json.dumps({
    'evidence': 'diagnostic-variants-only', 'source_sha256': hashlib.sha256(pe.data).hexdigest(), 'observations': observations}, indent=2))
print(json.dumps(observations, indent=2))
