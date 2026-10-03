#!/usr/bin/env python3
"""Verify type-specific Swift consumed-input lifetimes, never generic noescape.

The compiler owns each complete typed temporary and ends its lifetime immediately
following the exact AnyHashable initializer. The result stays opaque. This
contract describes eight initialized input bytes which may be consumed/written;
it does not prove a caller's frame, a result layout, or purity.
"""
import argparse
import json
from pathlib import Path
import re
import tempfile

try:
    from .generate_darwin_declarations import export_index
    from .generate_swift_data_declarations import _normalized_body, run
    from .generate_swift_metadata_declarations import TARGETS, EXPORT_TARGETS
except ImportError:
    from generate_darwin_declarations import export_index
    from generate_swift_data_declarations import _normalized_body, run
    from generate_swift_metadata_declarations import TARGETS, EXPORT_TARGETS

SOURCE = '''@_silgen_name("neverd_box_uint")
public func boxUInt(_ value: UInt) -> AnyHashable { AnyHashable(value) }
@_silgen_name("neverd_box_identifier")
public func boxIdentifier(_ value: ObjectIdentifier) -> AnyHashable { AnyHashable(value) }
'''
RUNTIME = '$ss11AnyHashableVyABxcSHRzlufC'
METADATA = '$sSuN'
WITNESS = '$sSuSHsWP'
PROVIDER = '/usr/lib/swift/libswiftCore.dylib'
IDENTIFIER_METADATA = '$sSON'
IDENTIFIER_WITNESS = '$sSOSHsWP'


def _consumed_input(ir, probe, name, storage, carrier, metadata, witness):
    if len(ir) > 1024 * 1024:
        raise ValueError('consumed-input IR exceeds its budget')
    functions = re.findall(
        r'^define swiftcc void @' + re.escape(probe) + r'\(ptr noalias nocapture '
        r'sret\(%Ts11AnyHashableV\) (%[A-Za-z_0-9.]+), ' + carrier + r' (%[A-Za-z_0-9.]+)\)'
        r'(?: #[0-9]+)? \{\n(.*?)^\}', ir, re.M | re.S)
    headers = re.findall(r'^define [^\n]*@' + re.escape(probe) + r'[^\n]*$', ir, re.M)
    if len(functions) != 1 or len(headers) != 1:
        raise ValueError('missing or ambiguous ' + name + ' probe')
    for identity, expected in (
            (storage, 'type <{ ' + carrier + ' }>'),
            ('@"' + metadata + '"', 'external global %swift.type, align 8'),
            ('@"' + witness + '"', 'external global ptr, align 8')):
        if re.findall(r'^' + re.escape(identity) + r' = ([^\n]+)$', ir, re.M) != [expected]:
            raise ValueError(name + ' storage or external identity is incomplete')
    declarations = re.findall(r'^declare ([^\n]*@"' + re.escape(RUNTIME) + r'"[^\n]*)$', ir, re.M)
    declaration = 'swiftcc void @"' + RUNTIME + '"(ptr noalias nocapture sret(%Ts11AnyHashableV), ptr noalias, ptr, ptr)'
    if len(declarations) != 1 or not re.fullmatch(
            re.escape(declaration) + r'(?: local_unnamed_addr)?(?: #[0-9]+)?', declarations[0]):
        raise ValueError('runtime ABI does not match the complete declaration')
    result, value, body = functions[0]
    def canonical(body, output, value):
        identities = {output: '"entry.result"', value: '"entry.value"',
                      storage: '"type.' + name + '"',
                      '%Ts11AnyHashableV': '"type.AnyHashable"'}
        if len(identities) != 4:
            raise ValueError('ambiguous entry identity')
        body = re.sub(r'%[A-Za-z_0-9.]+', lambda m: identities.get(m[0], m[0]), body)
        return _normalized_body(body)
    expected = '''entry:
 %temporary = alloca STORAGE, align 8
 call void @llvm.lifetime.start.p0(i64 8, ptr nonnull %temporary)
 store CARRIER %value, ptr %temporary, align 8
 call swiftcc void @"RUNTIME"(ptr noalias nocapture sret(%Ts11AnyHashableV) %result, ptr noalias nonnull %temporary, ptr nonnull @"METADATA", ptr nonnull @"WITNESS")
 call void @llvm.lifetime.end.p0(i64 8, ptr nonnull %temporary)
 ret void
'''.replace('STORAGE', storage).replace('CARRIER', carrier).replace('RUNTIME', RUNTIME).replace('METADATA', metadata).replace('WITNESS', witness)
    if canonical(body, result, value) != canonical(expected, '%result', '%value'):
        raise ValueError('complete ' + name + ' initialization, call and lifetime are not proved')
    return (RUNTIME, metadata, witness, 8)


def consumed_uint_input(ir):
    return _consumed_input(ir, 'neverd_box_uint', 'UInt', '%TSu', 'i64', METADATA, WITNESS)


def consumed_identifier_input(ir):
    return _consumed_input(ir, 'neverd_box_identifier', 'ObjectIdentifier', '%TSO', 'ptr',
                           IDENTIFIER_METADATA, IDENTIFIER_WITNESS)


def render(profiles, exports, version, compiler):
    expected = ((RUNTIME, METADATA, WITNESS, 8),
                (RUNTIME, IDENTIFIER_METADATA, IDENTIFIER_WITNESS, 8))
    if len(profiles) != 4 or any(p != expected for p in profiles) or len(exports) != 4:
        raise ValueError('all four compiler profiles must prove both independent typed contracts')
    if any(PROVIDER not in exports[i].get(name, set()) for i in range(4)
           for contract in expected for name in contract[:3]):
        raise ValueError('every exact identity must be exported in every profile')
    return '\n'.join([
        '// clang-format off',
        '// Generated by scripts/generate_swift_consumed_inputs.py.',
        '// Complete UInt and ObjectIdentifier temporary/store/call/lifetime proofs: MacOSX SDK ' + version + '.',
        '// ' + compiler.replace('\n', '; '),
        '// Target profiles: ' + ', '.join(TARGETS),
        '// Only each independently typed initialized input may be consumed synchronously.',
        '// No generic input noescape, result layout, frame identity or purity.',
        *('{' + ', '.join(json.dumps(s) for s in (*contract[:3], PROVIDER)) + ', ' + str(contract[3]) + '},'
          for contract in expected),
        '    // clang-format on', ''])


def main():
    import yaml
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--swiftc', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    sdk = args.sdk.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='neverd-consumed-input-') as work:
        source = Path(work) / 'probe.swift'
        source.write_text(SOURCE)
        profiles = []
        for i, target in enumerate(TARGETS):
            ir = Path(work) / f'{i}.ll'
            run([str(args.swiftc), '-O', '-parse-as-library', '-target', target,
                 '-sdk', str(sdk), '-emit-ir', str(source), '-o', str(ir)])
            profiles.append((consumed_uint_input(ir.read_text()), consumed_identifier_input(ir.read_text())))
    class TBDLoader(yaml.SafeLoader):
        pass
    TBDLoader.add_constructor('!tapi-tbd', lambda loader, node:
                              loader.construct_mapping(node, deep=True))
    documents = list(yaml.load_all((sdk / 'usr/lib/swift/libswiftCore.tbd').read_text(), Loader=TBDLoader))
    exports = [export_index(documents, target) for target in EXPORT_TARGETS]
    output = render(profiles, exports, json.loads((sdk / 'SDKSettings.json').read_text())['Version'],
                    run([str(args.swiftc), '--version']).strip())
    if args.check:
        if args.output.read_text() != output:
            parser.error('generated consumed-input contracts differ')
    else:
        args.output.write_text(output)
    print('verified two independent typed consumed-input lifetime contracts')


if __name__ == '__main__':
    main()
