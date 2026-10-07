#!/usr/bin/env python3
"""Verify descriptor-specific unused witness-instantiation arguments.

Each probe covers every legal instantiation of its exact conformance. Complete
compiler bodies either preserve all metadata inputs and pass an undef third
operand to swift_getWitnessTable, or prove the fixed MainActor: Actor static
table. Swift 6.1.2's static/nondependent runtime paths do not consume the caller's
instantiation argument. This grants no purity, layout or frame effects.
"""
import argparse
import json
from pathlib import Path
import re
import tempfile

try:
    from .generate_darwin_declarations import export_index
    from .generate_swift_data_declarations import (
        GENERIC_RANGE_SOURCE, MAIN_ACTOR_SOURCE, _normalized_body,
        generic_range_conformance_storage, main_actor_static_conformance_storage, run)
    from .generate_swift_metadata_declarations import TARGETS, EXPORT_TARGETS
except ImportError:
    from generate_darwin_declarations import export_index
    from generate_swift_data_declarations import (
        GENERIC_RANGE_SOURCE, MAIN_ACTOR_SOURCE, _normalized_body,
        generic_range_conformance_storage, main_actor_static_conformance_storage, run)
    from generate_swift_metadata_declarations import TARGETS, EXPORT_TARGETS

SOURCE = '''import Combine
@_silgen_name("neverd_publisher_probe")
func observe<P: Publisher>(_ type: P.Type)
@_silgen_name("neverd_generic_publisher_probe")
public func generic<Output, Failure: Error>(_ type: CurrentValueSubject<Output, Failure>.Type) {
  observe(type)
}
'''


STRING_PROTOCOL_SOURCE = '''@_silgen_name("neverd_string_protocol_observer")
func observe<S: StringProtocol>(_ type: S.Type)
@_silgen_name("neverd_string_protocol_witness_probe")
public func stringProtocolWitness() {
  observe(String.self)
}
'''


def string_protocol_instantiation_argument(ir):
    """Prove String's complete fixed metatype query and lazy witness body."""
    if len(ir) > 1024 * 1024:
        raise ValueError('string witness IR exceeds its input budget')
    descriptor, metadata, cache = '$sSSSysMc', '$sSSN', '$sS2SSysWL'
    for name, expected in (
            (descriptor, 'external global %swift.protocol_conformance_descriptor, align 4'),
            (metadata, 'external global %swift.type, align 8'),
            (cache, 'linkonce_odr hidden local_unnamed_addr global ptr null, align 8')):
        declarations = re.findall(r'^@"' + re.escape(name) + r'" = ([^\n]+)$', ir, re.M)
        if declarations != [expected]:
            raise ValueError('changed string descriptor, metadata or cache storage')
    for name, prototype in (
            ('swift_getWitnessTable', 'ptr @swift_getWitnessTable(ptr, ptr, ptr)'),
            ('neverd_string_protocol_observer',
             'swiftcc void @neverd_string_protocol_observer(ptr, ptr, ptr)')):
        declarations = re.findall(r'^declare ([^\n]*@' + name + r'[^\n]*)$', ir, re.M)
        if len(declarations) != 1 or not re.fullmatch(
                re.escape(prototype) + r'(?: local_unnamed_addr)?(?: #[0-9]+)?', declarations[0]):
            raise ValueError('unexpected string witness/protocol ABI')
    probe = 'neverd_string_protocol_witness_probe'
    accessor = '"$sS2SSysWl"'
    bodies = (
        (probe, 'swiftcc void @' + probe + '()', '''entry:
 %witness = tail call ptr @"$sS2SSysWl"()
 tail call swiftcc void @neverd_string_protocol_observer(ptr nonnull @"$sSSN", ptr nonnull @"$sSSN", ptr %witness)
 ret void
'''),
        (accessor, 'linkonce_odr hidden ptr @' + accessor + '()', '''entry:
 %cached = load ptr, ptr @"$sS2SSysWL", align 8
 %missing = icmp eq ptr %cached, null
 br i1 %missing, label %cacheIsNull, label %cont
cacheIsNull:
 %witness = tail call ptr @swift_getWitnessTable(ptr nonnull @"$sSSSysMc", ptr nonnull @"$sSSN", ptr undef)
 store atomic ptr %witness, ptr @"$sS2SSysWL" release, align 8
 br label %cont
cont:
 %result = phi ptr [ %cached, %entry ], [ %witness, %cacheIsNull ]
 ret ptr %result
'''))
    for name, prototype, expected in bodies:
        headers = re.findall(r'^define [^\n]*@' + re.escape(name) + r'[^\n]*$', ir, re.M)
        functions = re.findall(r'^define ' + re.escape(prototype) +
                               r'(?: local_unnamed_addr)?(?: #[0-9]+)? \{\n(.*?)^\}',
                               ir, re.M | re.S)
        if len(headers) != 1 or len(functions) != 1 or \
                _normalized_body(functions[0]) != _normalized_body(expected):
            raise ValueError('string witness probe or accessor flow is incomplete')
    return descriptor


def main_actor_instantiation_argument(ir):
    """Static MainActor: Actor has no caller-supplied instantiation inputs.

    The shared reader verifies the complete fixed metadata/static-table flow.
    Swift 6.1.2 Metadata.cpp's swift_getWitnessTable fast path returns the
    static table, or instantiates a nondependent table with nullptr, retaining
    runtime lookup and cache effects. This is not a generic Actor contract.
    """
    return next(name for name in main_actor_static_conformance_storage(ir)
                if name.endswith('Mc'))


def unused_instantiation_argument(ir):
    if len(ir) > 1024 * 1024:
        raise ValueError('witness contract IR exceeds its input budget')
    functions = re.findall(
        r'^define swiftcc void @neverd_generic_publisher_probe\(ptr '
        r'(%[A-Za-z_0-9.]+)\)(?: #[0-9]+)? \{\n(.*?)^\}',
        ir, re.M | re.S)
    headers = re.findall(r'^define [^\n]*@neverd_generic_publisher_probe[^\n]*$', ir, re.M)
    if len(functions) != 1 or len(headers) != 1:
        raise ValueError('missing or ambiguous generic metatype probe')
    for name, declaration in (
            ('swift_getWitnessTable', 'ptr @swift_getWitnessTable(ptr, ptr, ptr)'),
            ('neverd_publisher_probe', 'swiftcc void @neverd_publisher_probe(ptr, ptr, ptr)')):
        declarations = re.findall(r'^declare ([^\n]*@' + name + r'[^\n]*)$', ir, re.M)
        if len(declarations) != 1 or not re.fullmatch(
                re.escape(declaration) + r'(?: local_unnamed_addr)?(?: #[0-9]+)?',
                declarations[0]):
            raise ValueError('unexpected witness/protocol call ABI')
    parameter, body = functions[0]
    descriptors = re.findall(
        r'@swift_getWitnessTable\(ptr nonnull @"([^"\n]+)"', body)
    if len(descriptors) != 1:
        raise ValueError('missing unique descriptor input')
    descriptor = descriptors[0]
    declarations = re.findall(r'^@"' + re.escape(descriptor) + r'" = ([^\n]+)$', ir, re.M)
    if declarations != ['external global %swift.protocol_conformance_descriptor, align 4']:
        raise ValueError('descriptor is not exact direct external storage')
    expected = '''entry:
 %witness = tail call ptr @swift_getWitnessTable(ptr nonnull @"DESCRIPTOR", ptr %type, ptr undef)
 tail call swiftcc void @neverd_publisher_probe(ptr %type, ptr %type, ptr %witness)
 ret void
'''.replace('DESCRIPTOR', descriptor)
    if _normalized_body(body, parameter) != _normalized_body(expected, '%type'):
        raise ValueError('generic witness flow or unused argument is not proved')
    return descriptor


def render(profiles, exports, version, compiler):
    profiles = [{profile} if isinstance(profile, str) else set(profile)
                for profile in profiles]
    if len(profiles) != 4 or len(exports) != 4 or not profiles[0] or any(
            profile != profiles[0] for profile in profiles[1:]):
        raise ValueError('all four generic compiler profiles must agree')
    if '$sScMScAsMc' in profiles[0]:
        provider = '/usr/lib/swift/libswift_Concurrency.dylib'
        if any(any(provider not in export.get(name, set())
                   for name in ('$sScMScAsMc', '$sScMScAsWP', '$sScMMa'))
               for export in exports):
            raise ValueError('all main actor metadata/conformance/table exports are required')
    lines = [
        '// clang-format off',
        '// Generated by scripts/generate_swift_witness_contracts.py.',
        f'// Complete witness-query proof: MacOSX SDK {version}.',
        '// ' + compiler.replace('\n', '; '),
        '// Target profiles: ' + ', '.join(TARGETS),
        '// Only swift_getWitnessTable argument 2 is unused for these conformances.',
        '// No descriptor layout, purity, frame borrowing or value-witness ABI.',
        '// Fixed MainActor: Actor: complete static SDK table and nondependent runtime path.',
        '// https://github.com/swiftlang/swift/blob/swift-6.1.2-RELEASE/stdlib/public/runtime/Metadata.cpp#L5980',
    ]
    for name in sorted(profiles[0]):
        modules = [exports[i].get(name, set()) & exports[i + 1].get(name, set())
                   for i in (0, 2)]
        if not all(modules):
            raise ValueError('descriptor must be exported in all four SDK profiles')
        lines.append('{' + ', '.join(json.dumps(x) for x in
                                   (name, *('|'.join(sorted(m)) for m in modules))) + '},')
    return '\n'.join(lines + ['    // clang-format on', ''])


def main():
    import yaml
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--swiftc', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    sdk = args.sdk.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='neverd-witness-contract-') as work:
        source = Path(work) / 'probe.swift'
        source.write_text(SOURCE)
        range_source = Path(work) / 'range.swift'
        range_source.write_text(GENERIC_RANGE_SOURCE)
        string_source = Path(work) / 'string.swift'
        string_source.write_text(STRING_PROTOCOL_SOURCE)
        actor_source = Path(work) / 'main-actor.swift'
        actor_source.write_text(MAIN_ACTOR_SOURCE)
        profiles = []
        for index, target in enumerate(TARGETS):
            ir = Path(work) / f'{index}.ll'
            run([str(args.swiftc), '-O', '-parse-as-library', '-target', target,
                 '-sdk', str(sdk), '-emit-ir', str(source), '-o', str(ir)])
            range_ir = Path(work) / f'range-{index}.ll'
            run([str(args.swiftc), '-O', '-parse-as-library', '-target', target,
                 '-sdk', str(sdk), '-emit-ir', str(range_source), '-o', str(range_ir)])
            string_ir = Path(work) / f'string-{index}.ll'
            run([str(args.swiftc), '-O', '-parse-as-library', '-target', target,
                 '-sdk', str(sdk), '-emit-ir', str(string_source), '-o', str(string_ir)])
            actor_ir = Path(work) / f'main-actor-{index}.ll'
            run([str(args.swiftc), '-O', '-parse-as-library', '-target', target,
                 '-sdk', str(sdk), '-emit-ir', str(actor_source), '-o', str(actor_ir)])
            profiles.append({unused_instantiation_argument(ir.read_text()),
                             string_protocol_instantiation_argument(string_ir.read_text()),
                             main_actor_instantiation_argument(actor_ir.read_text())} |
                            generic_range_conformance_storage(range_ir.read_text()))
    class TBDLoader(yaml.SafeLoader):
        pass
    TBDLoader.add_constructor('!tapi-tbd', lambda loader, node:
                              loader.construct_mapping(node, deep=True))
    documents = []
    for tbd in ('System/Library/Frameworks/Combine.framework/Versions/A/Combine.tbd',
                'usr/lib/swift/libswift_Concurrency.tbd',
                'usr/lib/swift/libswiftCore.tbd'):
        documents.extend(yaml.load_all((sdk / tbd).read_text(), Loader=TBDLoader))
    exports = [export_index(documents, target) for target in EXPORT_TARGETS]
    output = render(profiles, exports,
                    json.loads((sdk / 'SDKSettings.json').read_text())['Version'],
                    run([str(args.swiftc), '--version']).strip())
    if args.check:
        if args.output.read_text() != output:
            parser.error('generated witness contracts differ')
    else:
        args.output.write_text(output)
    print('verified four descriptor-specific witness contracts')


if __name__ == '__main__':
    main()
