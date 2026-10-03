#!/usr/bin/env python3
"""Verify bounded, typed AnyHashable lifetimes without initializing padding.

The complete compiler-owned stack lifetime and its outlined value witnesses
own this contract. It grants no caller frame, physical ABI, purity or source
publication permission. In particular, a valid value is not initialized bytes.
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

SOURCE = '''@_silgen_name("neverd_anyhashable_copy")
public func copyAnyHashable(_ source: UnsafePointer<AnyHashable>, _ target: UnsafeMutablePointer<AnyHashable>) {
  target.initialize(to: source.pointee)
}
@_silgen_name("neverd_anyhashable_destroy")
public func destroyAnyHashable(_ value: UnsafeMutablePointer<AnyHashable>) {
  value.deinitialize(count: 1)
}
@_silgen_name("neverd_anyhashable_equal")
public func equalAnyHashable(_ first: AnyHashable, _ second: AnyHashable) -> Bool {
  first == second
}
@_silgen_name("neverd_anyhashable_bounded_lifetime")
public func boundedLifetimeAnyHashable(_ source: inout AnyHashable, _ body: (inout AnyHashable, AnyHashable) -> Bool) -> Bool {
  let copy = source
  return body(&source, copy)
}
'''
METADATA = '$ss11AnyHashableVN'
COPY = '$ss11AnyHashableVWOc'
DESTROY = '$ss11AnyHashableVWOh'
EQUAL = '$ss11AnyHashableV2eeoiySbAB_ABtFZ'
PROVIDER = '/usr/lib/swift/libswiftCore.dylib'
LOCAL = r'%(?:[A-Za-z_0-9.]+|"[^"\\\n]+")'


def _body(ir, symbol, signature):
    name = '@"' + symbol + '"' if symbol.startswith('$') else '@' + symbol
    headers = re.findall(r'^define [^\n]*' + re.escape(name) + r'\([^\n]*$', ir, re.M)
    pattern = (r'^define ' + signature + r'(?: local_unnamed_addr)?(?: #[0-9]+)?'
               r' \{\n(.*?)^\}')
    matches = re.findall(pattern.replace('@FUNCTION', re.escape(name)), ir, re.M | re.S)
    if len(headers) != 1 or len(matches) != 1:
        raise ValueError('missing, ambiguous or incomplete function: ' + symbol)
    return matches[0]


def _canonical(body, parameters):
    # Preserve globals and named type identity; only local SSA names may vary.
    names = {'%Ts11AnyHashableV': '"type.AnyHashable"'}
    for index, name in enumerate(parameters):
        if name in names:
            raise ValueError('ambiguous parameter identity')
        names[name] = '"entry.' + str(index) + '"'

    def local(match):
        name = match[0]
        if name not in names:
            names[name] = '%local' + str(len(names))
        return names[name]

    return _normalized_body(re.sub(LOCAL, local, body))


def opaque_value_contract(ir):
    if len(ir) > 1024 * 1024:
        raise ValueError('opaque value IR exceeds its budget')
    if re.findall(r'^@"' + re.escape(METADATA) + r'" = ([^\n]+)$', ir, re.M) != [
            'external global %swift.type, align 8']:
        raise ValueError('metadata is not a unique direct external identity')
    # The compiler's temporary, not host MemoryLayout, supplies this extent.
    types = {'%Ts11AnyHashableV': 'type <{ %Ts15_AnyHashableBoxP }>',
             '%Ts15_AnyHashableBoxP': 'type { [24 x i8], ptr, ptr }'}
    for name, value in types.items():
        if re.findall(r'^' + re.escape(name) + r' = ([^\n]+)$', ir, re.M) != [value]:
            raise ValueError('incomplete compiler temporary type')

    def witness(symbol, arguments, expected):
        signature = r'linkonce_odr hidden ptr @FUNCTION\(' + ', '.join(
            'ptr (' + LOCAL + ')' for _ in range(arguments)) + r'\)'
        parts = _body(ir, symbol, signature)
        body = parts[-1]
        for name, value in [('invariant.load', '!{}'), ('dereferenceable', '!{i64 88}')]:
            ids = re.findall(r', !' + re.escape(name) + r' !([0-9]+)', body)
            if len(ids) != (2 if name == 'invariant.load' else 1):
                raise ValueError('incomplete value witness load metadata')
            for identifier in ids:
                if re.findall(r'^!' + identifier + r' = ([^\n]+)$', ir, re.M) != [value]:
                    raise ValueError('stale value witness load metadata')
            body = re.sub(r', !' + re.escape(name) + r' ![0-9]+', '', body)
        attributes = re.findall(r' #[0-9]+\b', body)
        if len(attributes) != 1 or re.findall(
                r'^attributes ' + attributes[0].strip() + r' = ([^\n]+)$', ir, re.M) != ['{ nounwind }']:
            raise ValueError('unexpected value witness unwind contract')
        if _canonical(body, parts[:-1]) != _canonical(expected, ['%source', '%target'][:arguments]):
            raise ValueError('complete dynamic value witness body differs')

    witness(COPY, 2, '''entry:
 %vwt = load ptr, ptr getelementptr inbounds (i8, ptr @"METADATA", i64 -8), align 8
 %slot = getelementptr inbounds i8, ptr %vwt, i64 16
 %operation = load ptr, ptr %slot, align 8
 %result = tail call ptr %operation(ptr noalias %target, ptr noalias %source, ptr nonnull @"METADATA")
 ret ptr %target
'''.replace('METADATA', METADATA))
    witness(DESTROY, 1, '''entry:
 %vwt = load ptr, ptr getelementptr inbounds (i8, ptr @"METADATA", i64 -8), align 8
 %slot = getelementptr inbounds i8, ptr %vwt, i64 8
 %operation = load ptr, ptr %slot, align 8
 tail call void %operation(ptr noalias %source, ptr nonnull @"METADATA")
 ret ptr %source
'''.replace('METADATA', METADATA))

    signature = (r'swiftcc i1 @FUNCTION\(ptr nocapture dereferenceable\(40\) (' + LOCAL +
                 r'), ptr nocapture readonly (' + LOCAL + r'), ptr (' + LOCAL + r')\)')
    parts = _body(ir, 'neverd_anyhashable_bounded_lifetime', signature)
    expected = '''entry:
 %temporary = alloca %Ts11AnyHashableV, align 8
 call void @llvm.lifetime.start.p0(i64 40, ptr nonnull %temporary)
 %copied = call ptr @"COPY"(ptr nonnull %source, ptr nonnull %temporary)
 %answer = call swiftcc i1 %body(ptr nocapture nonnull dereferenceable(40) %source, ptr noalias nocapture nonnull dereferenceable(40) %temporary, ptr swiftself %context)
 %destroyed = call ptr @"DESTROY"(ptr nonnull %temporary)
 call void @llvm.lifetime.end.p0(i64 40, ptr nonnull %temporary)
 ret i1 %answer
'''.replace('COPY', COPY).replace('DESTROY', DESTROY)
    if _canonical(parts[-1], parts[:-1]) != _canonical(expected, ['%source', '%body', '%context']):
        raise ValueError('complete typed initialize/use/destroy/lifetime sequence differs')

    signature = (r'swiftcc i1 @FUNCTION\(ptr noalias nocapture dereferenceable\(40\) (' + LOCAL +
                 r'), ptr noalias nocapture dereferenceable\(40\) (' + LOCAL + r')\)')
    parts = _body(ir, 'neverd_anyhashable_equal', signature)
    expected = '''entry:
 %answer = tail call swiftcc i1 @"EQUAL"(ptr noalias nocapture nonnull dereferenceable(40) %first, ptr noalias nocapture nonnull dereferenceable(40) %second)
 ret i1 %answer
'''.replace('EQUAL', EQUAL)
    if _canonical(parts[-1], parts[:-1]) != _canonical(expected, ['%first', '%second']):
        raise ValueError('equality does not borrow the same two typed values')
    declaration = ('swiftcc i1 @"' + EQUAL + '"(ptr noalias nocapture dereferenceable(40), '
                   'ptr noalias nocapture dereferenceable(40))')
    declarations = re.findall(r'^declare ([^\n]*@"' + re.escape(EQUAL) + r'"[^\n]*)$', ir, re.M)
    if len(declarations) != 1 or not re.fullmatch(
            re.escape(declaration) + r'(?: local_unnamed_addr)?(?: #[0-9]+)?', declarations[0]):
        raise ValueError('complete equality declaration differs')
    return (METADATA, EQUAL, 40, 16, 8)


def render(profiles, exports, version, compiler):
    expected = (METADATA, EQUAL, 40, 16, 8)
    if len(profiles) != 4 or any(p != expected for p in profiles) or len(exports) != 4:
        raise ValueError('all four profiles must prove the same typed lifetime')
    if any(PROVIDER not in exports[i].get(name, set()) for i in range(4)
           for name in (METADATA, EQUAL)):
        raise ValueError('metadata and equality must have exact exports in every profile')
    return '\n'.join([
        '// clang-format off',
        '// Generated by scripts/generate_swift_opaque_values.py.',
        '// Complete opaque copy/read/destroy/lifetime proof: MacOSX SDK ' + version + '.',
        '// ' + compiler.replace('\n', '; '),
        '// Target profiles: ' + ', '.join(TARGETS),
        '// Typed value extent only: padding is not initialized, no frame or purity grant.',
        '{' + ', '.join(json.dumps(s) for s in (METADATA, EQUAL, PROVIDER)) + ', 40, 16, 8},',
        '// clang-format on', ''])


def main():
    import yaml
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--swiftc', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    sdk = args.sdk.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='neverd-opaque-value-') as work:
        source = Path(work) / 'probe.swift'
        source.write_text(SOURCE)
        profiles = []
        for i, target in enumerate(TARGETS):
            ir = Path(work) / f'{i}.ll'
            run([str(args.swiftc), '-O', '-parse-as-library', '-target', target,
                 '-sdk', str(sdk), '-emit-ir', str(source), '-o', str(ir)])
            profiles.append(opaque_value_contract(ir.read_text()))
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
            parser.error('generated opaque-value contracts differ')
    else:
        args.output.write_text(output)
    print('verified one complete opaque value lifetime contract')


if __name__ == '__main__':
    main()
