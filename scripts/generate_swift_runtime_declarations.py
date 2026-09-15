#!/usr/bin/env python3
"""Extract fixed pointer/size runtime ABI facts from pinned Swift runtime declarations.

Only unconditional, directly written FUNCTION records are inputs. Macro-generated
families, unknown representations and unsupported calling conventions supply no facts.
The output contains declarations, not Swift runtime implementations.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


REVISION = "9215272a4725957dfabdd14e1ca76c0dfa4a3003"
SOURCE = ("https://github.com/swiftlang/swift/blob/" + REVISION +
          "/include/swift/Runtime/RuntimeFunctions.def")
SOURCE_SHA256 = "f0bd275c8f6a9a1ec038c41c79a2b5e06ea6e7b6988f907263c0c1613f12707c"
# The runtime declaration DSL omits the custom swiftself/swifterror parameters
# added for this entry by IRGenModule.cpp at the same pinned revision (1127-1140).
# Ordinary fixed Swift calls cannot represent that ABI.
CUSTOM_PARAMETER_ABIS = frozenset({'swift_willThrow'})
SWIFT_AVAILABILITIES = frozenset({
    'AlwaysAvailable', 'SignedDescriptorAvailability',
    'GetTypesInAbstractMetadataStateAvailability',
})
POINTERS = frozenset({
    "OpaquePtrTy", "TypeMetadataPtrTy", "RefCountedPtrTy", "Int8PtrTy", "PtrTy",
    "ObjCClassPtrTy", "BridgeObjectPtrTy", "ObjCPtrTy", "Int8PtrPtrTy",
    "UnknownRefCountedPtrTy", "ProtocolDescriptorPtrTy", "TypeContextDescriptorPtrTy",
    "ErrorPtrTy", "WitnessTablePtrTy", "ObjCBlockPtrTy",
    "ProtocolConformanceDescriptorPtrTy", "WitnessTablePtrPtrTy",
    "ProtocolRecordPtrTy", "RelativeAddressPtrTy", "TypeMetadataRecordPtrTy",
    "TypeMetadataPtrPtrTy", "OpenedErrorTriplePtrTy",
})


def unconditional_text(source):
    if len(source) > 2 * 1024 * 1024:
        raise ValueError("runtime declarations exceed input limit")
    source = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
    depth, continuation, lines = 0, False, []
    for line in source.splitlines():
        if continuation:
            continuation = line.rstrip().endswith('\\')
            continue
        if line.lstrip().startswith('#'):
            words = line.lstrip()[1:].strip().split(None, 1)
            if not words:
                continue
            directive = words[0]
            if directive in ('if', 'ifdef', 'ifndef'):
                depth += 1
            elif directive == 'endif':
                depth -= 1
                if depth < 0:
                    raise ValueError("unbalanced preprocessor conditional")
            continuation = line.rstrip().endswith('\\')
            continue
        if not depth:
            lines.append(line)
    if depth or continuation:
        raise ValueError("incomplete preprocessor directive")
    return '\n'.join(lines)


def arguments(text, start):
    depth, begin, parts = 1, start, []
    for index in range(start, len(text)):
        char = text[index]
        if char == '(':
            depth += 1
            if depth > 16:
                raise ValueError("runtime declaration nesting exceeds limit")
        elif char == ')':
            depth -= 1
            if not depth:
                parts.append(text[begin:index].strip())
                return parts, index + 1
        elif char == ',' and depth == 1:
            parts.append(text[begin:index].strip())
            begin = index + 1
    raise ValueError("unterminated runtime declaration")


def types(field, macro):
    match = re.fullmatch(macro + r'\(([^()]*)\)', field, re.S)
    if not match:
        return None
    names = [part.strip() for part in match[1].split(',')] if match[1].strip() else []
    result = []
    for name in names:
        if name in POINTERS:
            result.append('p')
        elif name == 'SizeTy':
            result.append('z')
        elif name == 'VoidTy':
            result.append('v')
        else:
            return None
    return result


def declarations(source):
    text = unconditional_text(source)
    facts = {}
    for match in re.finditer(r'\bFUNCTION\s*\(', text):
        parts, _ = arguments(text, match.end())
        if len(parts) != 10:
            raise ValueError("unexpected runtime declaration fields")
        _, module, name, convention, availability, returns, parameters, attrs, _, _ = parts
        if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', name):
            continue
        result, args = types(returns, 'RETURNS'), types(parameters, 'ARGS')
        attributes = re.fullmatch(r'ATTRS\(([A-Za-z_0-9,\s]*)\)', attrs)
        no_return = bool(attributes and 'NoReturn' in {a.strip() for a in attributes[1].split(',')})
        fact = None
        supported_c = convention == 'C_CC' and availability == 'AlwaysAvailable'
        supported_swift = (convention == 'SwiftCC' and
                           availability in SWIFT_AVAILABILITIES and
                           attributes and
                           {a.strip() for a in attributes[1].split(',')} <=
                           {'NoUnwind', 'WillReturn', 'NoReturn'} and
                           args is not None and len(args) <= 8)
        if (module == 'Swift' and name not in CUSTOM_PARAMETER_ABIS and
                (supported_c or supported_swift) and result and len(result) == 1 and
                args is not None and len(args) <= 16 and 'v' not in args and
                (attributes or attrs == 'NO_ATTRS') and
                (not no_return or result == ['v'])):
            fact = ''.join(result + args), no_return, convention == 'SwiftCC'
        # A rejected alternative must veto a seemingly supported declaration.
        facts.setdefault(name, set()).add(fact)
    return {name: next(iter(values)) for name, values in sorted(facts.items())
            if len(values) == 1 and None not in values}


def render(facts, digest):
    lines = ['// clang-format off',
             '// Generated by scripts/generate_swift_runtime_declarations.py.',
             '// ABI facts derived from Swift RuntimeFunctions.def:',
             '// ' + SOURCE, '// SHA256: ' + digest,
             '// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors.',
             '// Apache-2.0 WITH Swift-exception; see LICENSES/Swift-Runtime-ABI.txt.',
             '// Changes: extract fixed pointer/size C and Swift ABI declarations, 2026-09-16.',
             '// Implementations and runtime effect assumptions are not included.',
             'static constexpr SwiftRuntimeDeclaration SwiftRuntimeDeclarations[] = {']
    for name, (signature, no_return, swift) in facts.items():
        lines.append('    {' + json.dumps(name) + ', ' + json.dumps(signature) +
                     ', ' + str(no_return).lower() + ', ' + str(swift).lower() + '},')
    return '\n'.join(lines + ['};', '// clang-format on', ''])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--source-sha256', required=True)
    parser.add_argument('--output', type=Path,
                        default=Path('lib/loader/Swift/SwiftRuntimeDeclarations.inc'))
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    data = args.input.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if args.source_sha256 != SOURCE_SHA256 or digest != SOURCE_SHA256:
        parser.error('runtime declaration content does not match the expected hash')
    facts = declarations(data.decode('utf-8'))
    if not facts:
        parser.error('runtime declarations supply no supported ABI facts')
    output = render(facts, digest)
    if args.check:
        if not args.output.exists() or args.output.read_text() != output:
            parser.error('generated Swift C declarations differ')
    else:
        args.output.write_text(output)
    print(f'{len(facts)} fixed Swift runtime declarations')


if __name__ == '__main__':
    main()
