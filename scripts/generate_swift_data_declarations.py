#!/usr/bin/env python3
"""Extract standard Swift metadata, witness, and conformance identities.

Direct external non-TLS globals and the exact paired descriptor of the fixed
MainActor: Actor static table qualify. Four Darwin compiler/export profiles
must agree; no runtime layout is inferred.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

try:
    from .generate_darwin_declarations import export_index
    from .generate_swift_metadata_declarations import TARGETS, EXPORT_TARGETS
except ImportError:
    from generate_darwin_declarations import export_index
    from generate_swift_metadata_declarations import TARGETS, EXPORT_TARGETS


METADATA_TYPES = ("Any", "AnyHashable", "ObjectIdentifier", "String", "Substring",
                  "Bool", "Int", "Int8", "Int16", "Int32", "Int64", "UInt",
                  "UInt8", "UInt16", "UInt32", "UInt64", "Float", "Double")
HASHABLE_TYPES = tuple(name for name in METADATA_TYPES
                       if name not in ("Any", "Substring"))

GENERIC_RANGE_SOURCE = '''@_silgen_name("neverd_range_probe")
func observe<R: RangeExpression>(_ type: R.Type)
@_silgen_name("neverd_generic_range_probe")
public func generic<Bound: Comparable>(_ type: Range<Bound>.Type) {
  observe(type)
}
'''
STRING_INDEX_RANGE_SOURCE = '''@_cdecl("metadata_StringIndexRange")
public func metadata_StringIndexRange() -> UnsafeRawPointer {
  unsafeBitCast(Range<String.Index>.self, to: UnsafeRawPointer.self)
}
'''
MAIN_ACTOR_SOURCE = '''@_silgen_name("neverd_actor_observer")
func observe<A: Actor>(_ type: A.Type)
@_silgen_name("neverd_main_actor_witness_probe")
public func mainActorWitness() {
  observe(MainActor.self)
}
'''


def hashable_value(name):
    if name == "ObjectIdentifier":
        return "Swift.ObjectIdentifier(Swift.Int.self)"
    return "Swift.AnyHashable(0)" if name == "AnyHashable" else f"Swift.{name}()"


def metadata_storage(ir, probes):
    if len(ir) > 16 * 1024 * 1024:
        raise ValueError("metadata storage IR exceeds the input budget")
    declarations = {}
    for name, value in re.findall(r'^@"([^"\n]+)" = ([^\n]+)$', ir, re.M):
        declarations.setdefault(name, []).append(value)
    result = set()
    for probe in probes:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", probe):
            raise ValueError("invalid probe identifier")
        bodies = re.findall(r'^define [^\n]*@' + probe +
                            r'\(\)[^\n]*\{\n(.*?)^\}', ir, re.M | re.S)
        if len(bodies) != 1:
            raise ValueError("missing or ambiguous metadata storage probe")
        body = [line.strip() for line in bodies[0].splitlines()
                if line.strip() and not line.lstrip().startswith(';')]
        if body and body[0] == 'entry:':
            body.pop(0)
        direct = (re.fullmatch(r'ret ptr @"([^"\n]+)"', body[0])
                  if len(body) == 1 else None)
        # Any.self is the metadata member embedded eight bytes into Swift's
        # exported full-existential storage, rather than the storage base.
        existential = (re.fullmatch(
            r'ret ptr getelementptr inbounds \(i8, ptr @"([^"\n]+)", i64 8\)',
            body[0]) if len(body) == 1 else None)
        returned = direct or existential
        if not returned:
            continue
        name = returned[1]
        values = declarations.get(name, [])
        expected = ('external global %swift.type, align 8' if direct else
                    'external global %swift.full_existential_type')
        if len(values) == 1 and values[0] == expected:
            result.add(name)
    return result


def witness_storage(ir, probes, metadata):
    """Read the witness argument of a compiler-generated generic Hashable call.

    The Swift source owns the generic constraint and call ABI. Only its exact
    direct global argument qualifies; this grants no witness-table layout or
    permission to call a witness member.
    """
    if len(ir) > 16 * 1024 * 1024:
        raise ValueError("witness storage IR exceeds the input budget")
    declarations = {}
    for name, value in re.findall(r'^@"([^"\n]+)" = ([^\n]+)$', ir, re.M):
        declarations.setdefault(name, []).append(value)
    callee = re.findall(r'^declare ([^\n]*@neverd_hashable_probe[^\n]*)$', ir, re.M)
    if len(callee) != 1 or not re.fullmatch(
            r'swiftcc void @neverd_hashable_probe\(ptr noalias, ptr, ptr\)'
            r'(?: local_unnamed_addr)?(?: #[0-9]+)?', callee[0]):
        raise ValueError("generic witness probe has an unexpected call ABI")
    result = set()
    for probe in probes:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", probe):
            raise ValueError("invalid probe identifier")
        bodies = re.findall(r'^define [^\n]*@' + probe +
                            r'\(\)[^\n]*\{\n(.*?)^\}', ir, re.M | re.S)
        if len(bodies) != 1:
            raise ValueError("missing or ambiguous witness storage probe")
        calls = [line.strip() for line in bodies[0].splitlines()
                 if '@neverd_hashable_probe' in line]
        call = re.fullmatch(
            r'call swiftcc void @neverd_hashable_probe\('
            r'ptr noalias nonnull %[A-Za-z_0-9.]+, '
            r'ptr nonnull @"([^"\n]+)", ptr nonnull @"([^"\n]+)"\)'
            r'(?: #[0-9]+)?', calls[0]) if len(calls) == 1 else None
        if not call or call[1] not in metadata:
            continue
        values = declarations.get(call[2], [])
        if len(values) == 1 and values[0] == 'external global ptr, align 8':
            result.add(call[2])
    return result


def conformance_storage(ir, probes, metadata, protocol="StringProtocol"):
    """Read descriptors used by compiler-generated lazy witness accessors.

    The named probe must call a zero-argument accessor and pass its result to
    the declared generic protocol probe. The accessor must in turn pass
    direct descriptor and metadata globals to swift_getWitnessTable and cache
    the result with a release store. This extracts external storage identity;
    it grants no descriptor or witness-table layout.
    """
    if len(ir) > 16 * 1024 * 1024:
        raise ValueError("conformance storage IR exceeds the input budget")
    declarations = {}
    for name, value in re.findall(r'^@"([^"\n]+)" = ([^\n]+)$', ir, re.M):
        declarations.setdefault(name, []).append(value)
    runtime = re.findall(r'^declare ([^\n]*@swift_getWitnessTable[^\n]*)$',
                         ir, re.M)
    if len(runtime) != 1 or not re.fullmatch(
            r'ptr @swift_getWitnessTable\(ptr, ptr, ptr\)'
            r'(?: local_unnamed_addr)?(?: #[0-9]+)?', runtime[0]):
        raise ValueError("witness accessor has an unexpected runtime ABI")
    generic_name = {"StringProtocol": "neverd_string_protocol_probe",
                    "Sequence": "neverd_sequence_probe",
                    "CVarArg": "neverd_cvararg_probe"}.get(protocol)
    if not generic_name:
        raise ValueError("unsupported conformance probe")
    generic = re.findall(
        r'^declare ([^\n]*@' + generic_name + r'[^\n]*)$', ir, re.M)
    if len(generic) != 1 or not re.fullmatch(
            r'swiftcc void @' + generic_name + r'\(ptr noalias, ptr, ptr\)'
            r'(?: local_unnamed_addr)?(?: #[0-9]+)?', generic[0]):
        raise ValueError(protocol + " probe has an unexpected call ABI")

    result = set()
    for probe in probes:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", probe):
            raise ValueError("invalid probe identifier")
        bodies = re.findall(r'^define [^\n]*@' + probe +
                            r'\(\)[^\n]*\{\n(.*?)^\}', ir, re.M | re.S)
        if len(bodies) != 1:
            raise ValueError("missing or ambiguous conformance storage probe")
        wrapper = re.fullmatch(
            r'\s*entry:\s*tail call swiftcc void @"(\$s[^"\n]+)"\(\)'
            r'(?: #[0-9]+)?\s*ret void\s*', bodies[0])
        if wrapper:
            bodies = re.findall(
                r'^define swiftcc void @"' + re.escape(wrapper[1]) +
                r'"\(\)[^\n]*\{\n(.*?)^\}', ir, re.M | re.S)
            if len(bodies) != 1:
                continue
        getter_calls = re.findall(
            r'^\s*(%[A-Za-z_0-9.]+) = (?:tail )?call ptr '
            r'@"([^"\n]+)"\(\)(?: #[0-9]+)?$', bodies[0], re.M)
        generic_calls = re.findall(
            r'^\s*call swiftcc void @' + generic_name + r'\('
            r'ptr noalias nonnull %[A-Za-z_0-9.]+, '
            r'ptr nonnull @"([^"\n]+)", ptr (%[A-Za-z_0-9.]+)\)'
            r'(?: #[0-9]+)?$', bodies[0], re.M)
        if len(getter_calls) != 1 or len(generic_calls) != 1 or \
                generic_calls[0][0] not in metadata or \
                generic_calls[0][1] != getter_calls[0][0]:
            continue
        accessors = re.findall(
            r'^define [^\n]*@"' + re.escape(getter_calls[0][1]) +
            r'"\(\)[^\n]*\{\n(.*?)^\}', ir, re.M | re.S)
        if len(accessors) != 1:
            continue
        witness_calls = re.findall(
            r'^\s*(%[A-Za-z_0-9.]+) = tail call ptr '
            r'@swift_getWitnessTable\(ptr nonnull @"([^"\n]+)", '
            r'ptr nonnull @"([^"\n]+)", ptr undef\)(?: #[0-9]+)?$',
            accessors[0], re.M)
        if len(witness_calls) != 1 or witness_calls[0][2] != generic_calls[0][0]:
            continue
        stores = re.findall(
            r'^\s*store atomic ptr ' + re.escape(witness_calls[0][0]) +
            r', ptr @"([^"\n]+)" release, align 8$', accessors[0], re.M)
        loads = re.findall(
            r'^\s*%[A-Za-z_0-9.]+ = load ptr, ptr @"([^"\n]+)", align 8$',
            accessors[0], re.M)
        descriptor = witness_calls[0][1]
        values = declarations.get(descriptor, [])
        if len(stores) == 1 and stores[0] in loads and len(values) == 1 and \
                values[0] == ('external global '
                              '%swift.protocol_conformance_descriptor, align 4'):
            result.add(descriptor)
    return result


def _normalized_body(body, parameter=None, type_names=()):
    """Compare complete compiler bodies modulo SSA/label names and hints."""
    quoted = r'"(?:[^"\\\n]|\\.)*"'
    body = re.sub(quoted + r'|;[^\n]*',
                  lambda m: m[0] if m[0].startswith('"') else '', body)
    body = re.sub(r'(?m)^\s*([A-Za-z_0-9.]+):', r'%\1:', body)
    body = re.sub(quoted + r'| #[0-9]+\b|, !prof ![0-9]+\b',
                  lambda m: m[0] if m[0].startswith('"') else '', body)
    if isinstance(parameter, str):
        names = {parameter: '%arg'}
    else:
        names = {name: '%arg' + str(index)
                 for index, name in enumerate(parameter or ())}
    # Named LLVM types carry ABI identity; the range probe must not accept a
    # different aggregate by treating its type name as another SSA spelling.
    names.update({name: name for name in type_names})

    def rename(match):
        value = match[0]
        if value.startswith('"'):
            return value
        if value not in names:
            names[value] = '%v' + str(len(names))
        return names[value]

    renamed = re.sub(quoted + r'|%[A-Za-z_0-9.]+', rename, body)
    return ' '.join(re.findall(quoted + r'|[^"\s]+', renamed))


def generic_range_conformance_storage(ir):
    """Prove the complete Range<Bound: Comparable>: RangeExpression probe.

    Both generic inputs must reach Range's metadata accessor unchanged. The
    metadata response, witness query with undef argument 2 and constrained
    call must agree exactly. This proves direct external descriptor identity
    and argument irrelevance, not descriptor layout or runtime call effects.
    """
    if len(ir) > 1024 * 1024:
        raise ValueError('generic range IR exceeds its input budget')
    name = '$sSnyxGSXsMc'
    declarations = re.findall(r'^@"' + re.escape(name) + r'" = ([^\n]+)$',
                              ir, re.M)
    if declarations != [
            'external global %swift.protocol_conformance_descriptor, align 4']:
        raise ValueError('range descriptor is not exact direct external storage')
    if re.findall(r'^%swift.metadata_response = ([^\n]+)$', ir, re.M) != [
            'type { ptr, i64 }']:
        raise ValueError('unexpected range metadata response ABI')
    for symbol, declaration in (
            ('"$sSnMa"', 'swiftcc %swift.metadata_response @"$sSnMa"(i64, ptr, ptr)'),
            ('swift_getWitnessTable', 'ptr @swift_getWitnessTable(ptr, ptr, ptr)'),
            ('neverd_range_probe', 'swiftcc void @neverd_range_probe(ptr, ptr, ptr)')):
        rows = re.findall(r'^declare ([^\n]*@' + re.escape(symbol) +
                          r'[^\n]*)$', ir, re.M)
        if len(rows) != 1 or not re.fullmatch(
                re.escape(declaration) + r'(?: local_unnamed_addr)?(?: #[0-9]+)?',
                rows[0]):
            raise ValueError('unexpected generic range call ABI')
    functions = re.findall(
        r'^define swiftcc void @neverd_generic_range_probe\(ptr '
        r'(%[A-Za-z_0-9.]+), ptr (%[A-Za-z_0-9.]+)\)(?: #[0-9]+)? '
        r'\{\n(.*?)^\}', ir, re.M | re.S)
    headers = re.findall(r'^define [^\n]*@neverd_generic_range_probe[^\n]*$',
                         ir, re.M)
    if len(functions) != 1 or len(headers) != 1:
        raise ValueError('missing or ambiguous generic range probe')
    bound, comparable, body = functions[0]
    if bound == comparable:
        raise ValueError('range generic inputs must remain distinct')
    expected = '''entry:
 %response = tail call swiftcc %swift.metadata_response @"$sSnMa"(i64 0, ptr %bound, ptr %comparable)
 %metadata = extractvalue %swift.metadata_response %response, 0
 %witness = tail call ptr @swift_getWitnessTable(ptr nonnull @"$sSnyxGSXsMc", ptr %metadata, ptr undef)
 tail call swiftcc void @neverd_range_probe(ptr %metadata, ptr %metadata, ptr %witness)
 ret void
'''
    types = ('%swift.metadata_response',)
    if _normalized_body(body, (bound, comparable), types) != _normalized_body(
            expected, ('%bound', '%comparable'), types):
        raise ValueError('generic range metadata/witness flow is not proved')
    return {name}


def main_actor_static_conformance_storage(ir):
    """Verify the complete fixed MainActor: Actor query and its static table.

    The compiler passes the public table directly, without generic inputs or
    witness instantiation. Its exact paired conformance descriptor must also
    be exported in every SDK profile. Neither symbol supplies runtime layout.
    """
    if len(ir) > 1024 * 1024:
        raise ValueError('main actor IR exceeds its input budget')
    witness = '$sScMScAsWP'
    if re.findall(r'^@"' + re.escape(witness) + r'" = ([^\n]+)$',
                  ir, re.M) != ['external global ptr, align 8']:
        raise ValueError('main actor witness is not exact external storage')
    if re.findall(r'^%swift.metadata_response = ([^\n]+)$', ir, re.M) != [
            'type { ptr, i64 }']:
        raise ValueError('unexpected main actor metadata response ABI')
    for symbol, declaration in (
            ('"$sScMMa"', 'swiftcc %swift.metadata_response @"$sScMMa"(i64)'),
            ('neverd_actor_observer',
             'swiftcc void @neverd_actor_observer(ptr, ptr, ptr)')):
        rows = re.findall(r'^declare ([^\n]*@' + re.escape(symbol) +
                          r'[^\n]*)$', ir, re.M)
        if len(rows) != 1 or not re.fullmatch(
                re.escape(declaration) + r'(?: local_unnamed_addr)?(?: #[0-9]+)?',
                rows[0]):
            raise ValueError('unexpected main actor call ABI')
    name = 'neverd_main_actor_witness_probe'
    headers = re.findall(r'^define [^\n]*@' + name + r'[^\n]*$', ir, re.M)
    bodies = re.findall(r'^define swiftcc void @' + name +
                        r'\(\)(?: local_unnamed_addr)?(?: #[0-9]+)? '
                        r'\{\n(.*?)^\}', ir, re.M | re.S)
    expected = '''entry:
 %response = tail call swiftcc %swift.metadata_response @"$sScMMa"(i64 0)
 %metadata = extractvalue %swift.metadata_response %response, 0
 tail call swiftcc void @neverd_actor_observer(ptr %metadata, ptr %metadata, ptr nonnull @"$sScMScAsWP")
 ret void
'''
    if len(headers) != 1 or len(bodies) != 1 or _normalized_body(
            bodies[0], (), ('%swift.metadata_response',)) != _normalized_body(
                expected, (), ('%swift.metadata_response',)):
        raise ValueError('complete main actor static conformance flow is not proved')
    return {'$sScMScAsMc', witness}


def _pointer_function(ir, name, argument=False):
    symbol = '@"' + name + '"' if name.startswith('$') else '@' + name
    parameters = r'(ptr (%[A-Za-z_0-9.]+))' if argument else r'()()'
    matches = re.findall(
        r'^define (?:linkonce_odr hidden )?ptr ' + re.escape(symbol) +
        r'\(' + parameters + r'\)(?: local_unnamed_addr)?(?: #[0-9]+)?'
        r' \{\n(.*?)^\}', ir, re.M | re.S)
    return (matches[0][2], matches[0][1] or None) if len(matches) == 1 else None


# One complete shared transfer shape owns concrete/abstract metadata queries.
# This is a compiler-evidence reader, not permission to reconstruct metadata,
# rewrite runtime arguments or infer the contents of a foreign descriptor.
_INSTANTIATOR_BODY = '''entry:
  %cached = load atomic i64, ptr %arg monotonic, align 8
  %missing = icmp slt i64 %cached, 0
  br i1 %missing, label %create, label %done
done:
  %word = phi i64 [ %cached, %entry ], [ %new, %create ]
  %result = inttoptr i64 %word to ptr
  ret ptr %result
create:
  %negativeLength = ashr i64 %cached, 32
  %length = sub nsw i64 0, %negativeLength
  %lowBits = shl i64 %cached, 32
  %offset = ashr exact i64 %lowBits, 32
  %base = ptrtoint ptr %arg to i64
  %address = add i64 %offset, %base
  %name = inttoptr i64 %address to ptr
  %metadata = tail call swiftcc ptr @RUNTIME(ARGS)
  %new = ptrtoint ptr %metadata to i64
  store atomic i64 %new, ptr %arg monotonic, align 8
  br label %done
'''


def range_index_descriptor_storage(ir):
    """Extract String.Index's opaque descriptor from a complete type query.

    Authenticate the entire symbolic Range<String.Index> recipe, its private
    indirection cell, metadata record and runtime helper. Only the exported
    descriptor identity survives; the compiler's private layouts do not.
    """
    if len(ir) > 1024 * 1024:
        raise ValueError('range index IR exceeds its input budget')
    descriptor = '$sSS5IndexVMn'
    cell = 'got.' + descriptor
    recipe = 'symbolic Sny_____G SS5IndexV'
    record = '$sSnySS5IndexVGMD'
    packed = '<{ [3 x i8], i8, i32, [1 x i8], i8 }>'
    expected = {
        descriptor: 'external global %swift.type_descriptor, align 4',
        cell: 'private unnamed_addr constant ptr @"' + descriptor + '"',
        recipe: ('linkonce_odr hidden constant ' + packed + ' <{ '
                 '[3 x i8] c"Sny", i8 2, i32 trunc (i64 sub ('
                 'i64 ptrtoint (ptr @"' + cell + '" to i64), '
                 'i64 ptrtoint (ptr getelementptr inbounds (' + packed +
                 ', ptr @"' + recipe + '", i32 0, i32 2) to i64)) to i32), '
                 '[1 x i8] c"G", i8 0 }>, section "__TEXT,__swift5_typeref, regular", '
                 'no_sanitize_address, align 2'),
        record: ('linkonce_odr hidden global { i32, i32 } { i32 trunc ('
                 'i64 sub (i64 ptrtoint (ptr @"' + recipe + '" to i64), '
                 'i64 ptrtoint (ptr @"' + record + '" to i64)) to i32), '
                 'i32 -9 }, align 8'),
    }
    for name, value in expected.items():
        rows = re.findall(r'^@"' + re.escape(name) + r'" = ([^\n]+)$', ir, re.M)
        if rows != [value]:
            raise ValueError('incomplete or changed range descriptor storage: ' + name)
    runtime = 'swift_getTypeByMangledNameInContext2'
    declarations = re.findall(r'^declare ([^\n]*@' + runtime + r'[^\n]*)$', ir, re.M)
    declaration = 'swiftcc ptr @' + runtime + '(ptr, i64, ptr, ptr)'
    if len(declarations) != 1 or not re.fullmatch(
            re.escape(declaration) + r'(?: local_unnamed_addr)?(?: #[0-9]+)?',
            declarations[0]):
        raise ValueError('unexpected range metadata runtime ABI')
    helper = '__swift_instantiateConcreteTypeFromMangledName'
    function = _pointer_function(ir, helper, argument=True)
    headers = re.findall(r'^define [^\n]*@' + helper + r'[^\n]*$', ir, re.M)
    body = _INSTANTIATOR_BODY.replace('RUNTIME', runtime).replace(
        'ARGS', 'ptr %name, i64 %length, ptr null, ptr null')
    if len(headers) != 1 or not function or _normalized_body(*function) != _normalized_body(body, '%arg'):
        raise ValueError('range metadata helper is not complete')
    probe = 'metadata_StringIndexRange'
    query = _pointer_function(ir, probe)
    headers = re.findall(r'^define [^\n]*@' + probe + r'[^\n]*$', ir, re.M)
    body = ('entry:\n %metadata = tail call ptr @' + helper +
            '(ptr nonnull @"' + record + '")\n ret ptr %metadata')
    if len(headers) != 1 or not query or _normalized_body(query[0]) != _normalized_body(body):
        raise ValueError('range metatype query does not preserve its exact record')
    return {descriptor}


def generic_conformance_storage(ir, probes):
    """Extract opaque conformance addresses for compiler-instantiated types.

    Each pair names a metatype query and its Publisher-constraint probe. The
    query, generic call and complete lazy witness accessor must use the same
    uniquely defined metadata record. Both metadata helpers are checked in
    full, including request state, signed offsets, cache and returned value.
    Only the descriptor address is exported; no metadata/call-effect fact is.
    """
    if len(ir) > 16 * 1024 * 1024:
        raise ValueError("generic conformance IR exceeds the input budget")
    declarations = {}
    for name, value in re.findall(r'^@"([^"\n]+)" = ([^\n]+)$', ir, re.M):
        declarations.setdefault(name, []).append(value)
    prototypes = {
        'neverd_publisher_type_probe': ('swiftcc void', 'ptr, ptr, ptr'),
        'swift_getWitnessTable': ('ptr', 'ptr, ptr, ptr'),
        'swift_getTypeByMangledNameInContext2':
            ('swiftcc ptr', 'ptr, i64, ptr, ptr'),
        'swift_getTypeByMangledNameInContextInMetadataState2':
            ('swiftcc ptr', 'i64, ptr, i64, ptr, ptr'),
    }
    for name, (result, args) in prototypes.items():
        rows = re.findall(r'^declare ([^\n]*@' + name + r'[^\n]*)$', ir, re.M)
        expected = result + ' @' + name + '(' + args + ')'
        if len(rows) != 1 or not re.fullmatch(
                re.escape(expected) + r'(?: local_unnamed_addr)?(?: #[0-9]+)?',
                rows[0]):
            raise ValueError("generic conformance has an unexpected ABI: " + name)
    helper = '__swift_instantiateConcreteTypeFromMangledName'
    for suffix, runtime, args in [
        ('', 'swift_getTypeByMangledNameInContext2',
         'ptr %name, i64 %length, ptr null, ptr null'),
        ('Abstract', 'swift_getTypeByMangledNameInContextInMetadataState2',
         'i64 255, ptr %name, i64 %length, ptr null, ptr null'),
    ]:
        function = _pointer_function(ir, helper + suffix, argument=True)
        expected = _INSTANTIATOR_BODY.replace('RUNTIME', runtime).replace('ARGS', args)
        if not function or _normalized_body(*function) != \
                _normalized_body(expected, '%arg'):
            return set()

    result = set()
    for metadata_probe, witness_probe in probes:
        if not all(re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', name)
                   for name in (metadata_probe, witness_probe)):
            raise ValueError("invalid generic conformance probe identifier")
        query = _pointer_function(ir, metadata_probe)
        records = re.findall(re.escape('@' + helper) +
                             r'\(ptr nonnull @"([^"\n]+)"\)', query[0]) if query else []
        if len(records) != 1:
            continue
        record = records[0]
        values = declarations.get(record, [])
        # The record remains opaque. Exact shared identity, not a guessed
        # generic layout or copied initializer, links the two request states.
        if len(values) != 1 or not re.fullmatch(
                r'linkonce_odr hidden global \{ i32, i32 \} \{ [^\n]+ \}, align 8',
                values[0]):
            continue
        query_body = ('entry:\n %metadata = tail call ptr @' + helper +
                      '(ptr nonnull @"' + record + '")\n ret ptr %metadata')
        if _normalized_body(query[0]) != _normalized_body(query_body):
            continue
        bodies = re.findall(r'^define void @' + re.escape(witness_probe) +
                            r'\(\)(?: #[0-9]+)? \{\n(.*?)^\}', ir, re.M | re.S)
        if len(bodies) != 1:
            continue
        getters = re.findall(r'tail call ptr @"([^"\n]+)"\(\)', bodies[0])
        if len(getters) != 1:
            continue
        expected = (query_body[:query_body.index(' ret ptr')] +
                    '\n %witness = tail call ptr @"' + getters[0] + '"()\n'
                    ' tail call swiftcc void @neverd_publisher_type_probe('
                    'ptr %metadata, ptr %metadata, ptr %witness)\n ret void')
        if _normalized_body(bodies[0]) != _normalized_body(expected):
            continue
        accessor = _pointer_function(ir, getters[0])
        if not accessor:
            continue
        descriptors = re.findall(
            r'@swift_getWitnessTable\(ptr nonnull @"([^"\n]+)"', accessor[0])
        caches = re.findall(r'load ptr, ptr @"([^"\n]+)", align 8', accessor[0])
        if len(descriptors) != 1 or len(caches) != 1:
            continue
        descriptor, cache = descriptors[0], caches[0]
        expected = '''entry:
 %cached = load ptr, ptr @"CACHE", align 8
 %empty = icmp eq ptr %cached, null
 br i1 %empty, label %create, label %done
create:
 %metadata = tail call ptr @HELPERAbstract(ptr nonnull @"RECORD")
 %witness = tail call ptr @swift_getWitnessTable(ptr nonnull @"DESCRIPTOR", ptr %metadata, ptr undef)
 store atomic ptr %witness, ptr @"CACHE" release, align 8
 br label %done
done:
 %result = phi ptr [ %cached, %entry ], [ %witness, %create ]
 ret ptr %result
'''
        for before, after in [('CACHE', cache), ('HELPER', helper),
                              ('RECORD', record), ('DESCRIPTOR', descriptor)]:
            expected = expected.replace(before, after)
        if _normalized_body(accessor[0]) == _normalized_body(expected) and \
                declarations.get(cache) == [
                    'linkonce_odr hidden local_unnamed_addr global ptr null, align 8'] and \
                declarations.get(descriptor) == [
                    'external global %swift.protocol_conformance_descriptor, align 4']:
            result.add(descriptor)
    return result


def render(profiles, exports, version, compiler):
    if len(profiles) != 4 or len(exports) != 4:
        raise ValueError("all four compiler and export profiles are required")
    if any('$sScMScAsMc' in profile for profile in profiles):
        required = {'$sScMScAsMc', '$sScMScAsWP', '$sScMMa'}
        provider = '/usr/lib/swift/libswift_Concurrency.dylib'
        if any(not {'$sScMScAsMc', '$sScMScAsWP'} <= profile
               for profile in profiles) or any(
                any(provider not in export.get(name, set()) for name in required)
                for export in exports):
            raise ValueError('all main actor metadata/conformance/table exports are required')
    lines = ["// clang-format off",
             "// Generated by scripts/generate_swift_data_declarations.py.",
             f"// Compiler-derived external metadata/witness/conformance storage: MacOSX SDK {version}.",
             "// " + compiler.replace("\n", "; "),
             "// Target profiles: " + ", ".join(TARGETS),
             "// Direct non-TLS globals and the exact paired MainActor: Actor descriptor.",
             "// No runtime layout or implementation."]
    for name in sorted(set.intersection(*profiles)):
        modules = [exports[i].get(name, set()) & exports[i + 1].get(name, set())
                   for i in (0, 2)]
        if all(modules):
            lines.append("{" + ", ".join(json.dumps(x) for x in
                         (name, *("|".join(sorted(m)) for m in modules))) + "},")
    return "\n".join(lines + ["    // clang-format on", ""])


def run(command):
    return subprocess.run(command, check=True, capture_output=True, text=True,
                          timeout=180).stdout


def main():
    import yaml
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--swiftc', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    sdk = args.sdk.resolve(strict=True)
    probes = ['metadata_' + name for name in METADATA_TYPES]
    witnesses = ['witness_' + name for name in HASHABLE_TYPES]
    with tempfile.TemporaryDirectory(prefix='neverd-swift-data-') as work:
        source = Path(work) / 'metadata.swift'
        source.write_text('\n'.join(
            f'@_cdecl("{probe}") public func {probe}() -> UnsafeRawPointer {{ '
            f'unsafeBitCast({"Any" if name == "Any" else "Swift." + name}.self, '
            'to: UnsafeRawPointer.self) }'
            for probe, name in zip(probes, METADATA_TYPES)) + '\n' +
            '@_silgen_name("neverd_hashable_probe") '
            'func observe<T: Hashable>(_ value: T)\n' + '\n'.join(
                f'@_cdecl("{probe}") public func {probe}() {{ '
                f'observe({hashable_value(name)}) }}'
                for probe, name in zip(witnesses, HASHABLE_TYPES)) + '\n' +
            '@_silgen_name("neverd_string_protocol_probe") '
            'func observeStringProtocol<T: StringProtocol>(_ value: T)\n' +
            '@_cdecl("witness_StringProtocol") public func '
            'witness_StringProtocol() { observeStringProtocol("") }\n'
            '@_silgen_name("neverd_sequence_probe") '
            'func observeSequence<T: Sequence>(_ value: T)\n'
            '@_cdecl("witness_SubstringSequence") public func '
            'witness_SubstringSequence() { '
            'observeSequence(Swift.Substring()) }\n')
        # Keep protocol probes in separate compiler units: Swift can merge
        # their identical String bodies into an indirect shared thunk. The
        # extractor intentionally accepts only direct, authenticated calls.
        foundation_source = Path(work) / 'foundation.swift'
        foundation_source.write_text(
            'import Foundation\n'
            '@_cdecl("metadata_String") public func metadata_String() '
            '-> UnsafeRawPointer { unsafeBitCast(Swift.String.self, '
            'to: UnsafeRawPointer.self) }\n'
            '@_silgen_name("neverd_cvararg_probe") '
            'func observeCVarArg<T: CVarArg>(_ value: T)\n'
            '@_cdecl("witness_StringCVarArg") public func '
            'witness_StringCVarArg() { observeCVarArg(Swift.String()) }\n')
        combine_source = Path(work) / 'combine.swift'
        combine_source.write_text(
            'import Combine\n'
            '@_silgen_name("neverd_publisher_type_probe") '
            'func observe<P: Publisher>(_ value: P.Type)\n'
            '@_cdecl("metadata_CurrentValueSubject") public func '
            'metadata_CurrentValueSubject() -> UnsafeRawPointer { '
            'unsafeBitCast(CurrentValueSubject<Int, Never>.self, '
            'to: UnsafeRawPointer.self) }\n'
            '@_cdecl("witness_CurrentValueSubjectPublisher") public func '
            'witness_CurrentValueSubjectPublisher() { '
            'observe(CurrentValueSubject<Int, Never>.self) }\n')
        range_source = Path(work) / 'range.swift'
        range_source.write_text(GENERIC_RANGE_SOURCE)
        range_index_source = Path(work) / 'range-index.swift'
        range_index_source.write_text(STRING_INDEX_RANGE_SOURCE)
        actor_source = Path(work) / 'main-actor.swift'
        actor_source.write_text(MAIN_ACTOR_SOURCE)
        profiles = []
        for index, target in enumerate(TARGETS):
            ir = Path(work) / f'{index}.ll'
            run([str(args.swiftc), '-O', '-parse-as-library', '-target', target,
                 '-sdk', str(sdk), '-emit-ir', str(source), '-o', str(ir)])
            text = ir.read_text()
            metadata = metadata_storage(text, probes)
            foundation_ir = Path(work) / f'foundation-{index}.ll'
            run([str(args.swiftc), '-O', '-parse-as-library', '-target', target,
                 '-sdk', str(sdk), '-emit-ir', str(foundation_source),
                 '-o', str(foundation_ir)])
            foundation_text = foundation_ir.read_text()
            foundation_metadata = metadata_storage(foundation_text,
                                                   ['metadata_String'])
            combine_ir = Path(work) / f'combine-{index}.ll'
            run([str(args.swiftc), '-O', '-parse-as-library', '-target', target,
                 '-sdk', str(sdk), '-emit-ir', str(combine_source),
                 '-o', str(combine_ir)])
            range_ir = Path(work) / f'range-{index}.ll'
            run([str(args.swiftc), '-O', '-parse-as-library', '-target', target,
                 '-sdk', str(sdk), '-emit-ir', str(range_source),
                 '-o', str(range_ir)])
            range_index_ir = Path(work) / f'range-index-{index}.ll'
            run([str(args.swiftc), '-O', '-parse-as-library', '-target', target,
                 '-sdk', str(sdk), '-emit-ir', str(range_index_source),
                 '-o', str(range_index_ir)])
            actor_ir = Path(work) / f'main-actor-{index}.ll'
            run([str(args.swiftc), '-O', '-parse-as-library', '-target', target,
                 '-sdk', str(sdk), '-emit-ir', str(actor_source),
                 '-o', str(actor_ir)])
            profiles.append(metadata |
                            witness_storage(text, witnesses, metadata) |
                            conformance_storage(text, ['witness_StringProtocol'],
                                                metadata) |
                            conformance_storage(text,
                                                ['witness_SubstringSequence'], metadata,
                                                "Sequence") |
                            conformance_storage(foundation_text,
                                                ['witness_StringCVarArg'],
                                                foundation_metadata,
                                                "CVarArg") |
                            generic_conformance_storage(
                                combine_ir.read_text(),
                                [('metadata_CurrentValueSubject',
                                  'witness_CurrentValueSubjectPublisher')]) |
                            generic_range_conformance_storage(range_ir.read_text()) |
                            range_index_descriptor_storage(range_index_ir.read_text()) |
                            main_actor_static_conformance_storage(actor_ir.read_text()))
    class TBDLoader(yaml.SafeLoader):
        pass
    TBDLoader.add_constructor('!tapi-tbd', lambda loader, node:
                              loader.construct_mapping(node, deep=True))
    documents = []
    for tbd in ('usr/lib/swift/libswiftCore.tbd',
                'usr/lib/swift/libswift_Concurrency.tbd',
                'usr/lib/swift/libswiftFoundation.tbd',
                'System/Library/Frameworks/Combine.framework/Versions/A/Combine.tbd',
                'System/Library/Frameworks/Foundation.framework/Versions/C/Foundation.tbd'):
        documents.extend(yaml.load_all((sdk / tbd).read_text(), Loader=TBDLoader))
    exports = [export_index(documents, target) for target in EXPORT_TARGETS]
    output = render(profiles, exports,
                    json.loads((sdk / 'SDKSettings.json').read_text())['Version'],
                    run([str(args.swiftc), '--version']).strip())
    count = sum(line.startswith('{') for line in output.splitlines())
    expected = len(METADATA_TYPES) + len(HASHABLE_TYPES) + 8
    if count != expected:
        parser.error('not all standard storage queries have complete evidence '
                     f'({count}/{expected})')
    if args.check:
        if args.output.read_text() != output:
            parser.error('generated Swift data catalog differs')
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output)
    print(f'verified {count} external metadata/witness storage identities')


if __name__ == '__main__':
    main()
