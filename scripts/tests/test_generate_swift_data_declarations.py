import copy
from pathlib import Path
import re
import unittest

from scripts.generate_swift_data_declarations import (conformance_storage,
                                                        generic_conformance_storage,
                                                        HASHABLE_TYPES,
                                                        hashable_value,
                                                        METADATA_TYPES,
                                                        metadata_storage,
                                                        render,
                                                        witness_storage)


MODULE = '/usr/lib/swift/libswiftCore.dylib'
NAME = '$sSSN'
IR = ('@"$sSSN" = external global %swift.type, align 8\n'
      'define nonnull ptr @metadata_String() #0 {\nentry:\n'
      '  ret ptr @"$sSSN"\n}\n')
ANY = '$sypN'
ANY_IR = ('@"$sypN" = external global %swift.full_existential_type\n'
          'define nonnull ptr @metadata_Any() #0 {\nentry:\n'
          '  ret ptr getelementptr inbounds (i8, ptr @"$sypN", i64 8)\n}\n')
WITNESS = '$sSSSHsWP'
WITNESS_IR = ('@"$sSSSHsWP" = external global ptr, align 8\n'
              'declare swiftcc void @neverd_hashable_probe(ptr noalias, ptr, ptr) local_unnamed_addr #0\n'
              'define void @witness_String() #0 {\nentry:\n'
              '  %0 = alloca %TSS, align 8\n'
              '  call swiftcc void @neverd_hashable_probe(ptr noalias nonnull %0, '
              'ptr nonnull @"$sSSN", ptr nonnull @"$sSSSHsWP") #2\n'
              '  ret void\n}\n')
CONFORMANCE = '$sSSSysMc'
CONFORMANCE_IR = (
    '@"$sSSN" = external global %swift.type, align 8\n'
    '@"$sS2SSysWL" = linkonce_odr hidden local_unnamed_addr global ptr null, align 8\n'
    '@"$sSSSysMc" = external global %swift.protocol_conformance_descriptor, align 4\n'
    'declare swiftcc void @neverd_string_protocol_probe(ptr noalias, ptr, ptr) local_unnamed_addr #0\n'
    'declare ptr @swift_getWitnessTable(ptr, ptr, ptr) local_unnamed_addr #1\n'
    'define void @witness_StringProtocol() #0 {\nentry:\n'
    '  %0 = alloca i64, align 8\n'
    '  %1 = tail call ptr @"$sS2SSysWl"() #2\n'
    '  call swiftcc void @neverd_string_protocol_probe(ptr noalias nonnull %0, '
    'ptr nonnull @"$sSSN", ptr %1) #3\n'
    '  ret void\n}\n'
    'define linkonce_odr hidden ptr @"$sS2SSysWl"() local_unnamed_addr #2 {\nentry:\n'
    '  %0 = load ptr, ptr @"$sS2SSysWL", align 8\n'
    '  %1 = icmp eq ptr %0, null\n'
    '  br i1 %1, label %cacheIsNull, label %cont\n'
    'cacheIsNull:\n'
    '  %2 = tail call ptr @swift_getWitnessTable(ptr nonnull @"$sSSSysMc", '
    'ptr nonnull @"$sSSN", ptr undef) #4\n'
    '  store atomic ptr %2, ptr @"$sS2SSysWL" release, align 8\n'
    '  br label %cont\n'
    'cont:\n'
    '  %3 = phi ptr [ %0, %entry ], [ %2, %cacheIsNull ]\n'
    '  ret ptr %3\n}\n')
SUBSTRING_SEQUENCE = (CONFORMANCE_IR
                      .replace('$sSSSysMc', '$sSsSTsMc')
                      .replace('$sSSN', '$sSsN')
                      .replace('witness_StringProtocol',
                               'witness_SubstringSequence')
                      .replace('neverd_string_protocol_probe',
                               'neverd_sequence_probe'))
CVARARG = (CONFORMANCE_IR
           .replace('$sSSSysMc', '$sSSs7CVarArg10FoundationMc')
           .replace('$sS2SSysWl', '$sS2Ss7CVarArg10FoundationWl')
           .replace('$sS2SSysWL', '$sS2Ss7CVarArg10FoundationWL')
           .replace('witness_StringProtocol', 'witness_StringCVarArg')
           .replace('neverd_string_protocol_probe', 'neverd_cvararg_probe'))
PUBLISHER = '$s7Combine19CurrentValueSubjectCyxq_GAA9PublisherAAMc'
PUBLISHER_RECORD = '$s7Combine19CurrentValueSubjectCySis5NeverOGMD'
PUBLISHER_PROBES = [('metadata_CurrentValueSubject',
                     'witness_CurrentValueSubjectPublisher')]
PUBLISHER_IR = (Path(__file__).parent / 'fixtures' /
                'swift_publisher_conformance.ll').read_text()


class SwiftDataDeclarationTests(unittest.TestCase):
    def test_object_identifier_uses_metatype_value_and_direct_storage_probes(self):
        self.assertIn('ObjectIdentifier', METADATA_TYPES)
        self.assertIn('ObjectIdentifier', HASHABLE_TYPES)
        self.assertEqual(hashable_value('ObjectIdentifier'),
                         'Swift.ObjectIdentifier(Swift.Int.self)')
        metadata_ir = IR.replace('$sSSN', '$sSON').replace(
            'metadata_String', 'metadata_ObjectIdentifier')
        witness_ir = WITNESS_IR.replace('$sSSN', '$sSON').replace(
            '$sSSSHsWP', '$sSOSHsWP').replace(
            'witness_String', 'witness_ObjectIdentifier')
        self.assertEqual(metadata_storage(metadata_ir,
                                          ['metadata_ObjectIdentifier']),
                         {'$sSON'})
        self.assertEqual(witness_storage(witness_ir,
                                         ['witness_ObjectIdentifier'], {'$sSON'}),
                         {'$sSOSHsWP'})
        for before, after in [
            ('external global', 'external thread_local global'),
            ('external global', 'extern_weak global'),
            ('ptr nonnull @"$sSON"', 'ptr nonnull @"$sSuN"'),
            ('ptr nonnull @"$sSOSHsWP"', 'ptr %runtime'),
            ('align 8', 'align 4'),
        ]:
            with self.subTest(before=before, after=after):
                self.assertEqual(witness_storage(
                    witness_ir.replace(before, after),
                    ['witness_ObjectIdentifier'], {'$sSON'}), set())

    def test_generic_conformance_keeps_complete_compiler_type_query(self):
        self.assertEqual(generic_conformance_storage(PUBLISHER_IR,
                                                    PUBLISHER_PROBES),
                         {PUBLISHER})
        renamed = re.sub(r'%(\d+)\b', r'%renamed_\1', PUBLISHER_IR)
        renamed = re.sub(r'(?m)^(\d+):', r'renamed_\1:', renamed)
        self.assertEqual(generic_conformance_storage(renamed, PUBLISHER_PROBES),
                         {PUBLISHER})
        self.assertNotIn(PUBLISHER, metadata_storage(
            PUBLISHER_IR, ['metadata_CurrentValueSubject']))

    def test_generic_conformance_rejects_partial_changed_or_stale_evidence(self):
        changes = [
            ('external global %swift.protocol_conformance_descriptor',
             'external thread_local global %swift.protocol_conformance_descriptor'),
            ('external global %swift.protocol_conformance_descriptor',
             'extern_weak global %swift.protocol_conformance_descriptor'),
            ('external global %swift.protocol_conformance_descriptor',
             'global %swift.protocol_conformance_descriptor'),
            ('%swift.protocol_conformance_descriptor, align 4', 'ptr, align 4'),
            ('%swift.protocol_conformance_descriptor, align 4',
             '%swift.protocol_conformance_descriptor, align 8'),
            ('hidden global { i32, i32 }', 'hidden thread_local global { i32, i32 }'),
            ('hidden local_unnamed_addr global ptr null',
             'hidden local_unnamed_addr thread_local global ptr null'),
            ('load atomic i64', 'load i64'),
            ('monotonic, align 8', 'unordered, align 8'),
            ('icmp slt i64', 'icmp ult i64'),
            ('ashr i64 %1, 32', 'lshr i64 %1, 32'),
            ('sub nsw i64 0, %7', 'sub nsw i64 %7, 0'),
            ('ashr exact i64 %sext, 32', 'ashr exact i64 %sext, 16'),
            ('shl i64 %1, 32', 'shl i64 %1, 16'),
            ('add i64 %9, %10', 'sub i64 %9, %10'),
            ('inttoptr i64 %11 to ptr', 'inttoptr i64 %9 to ptr'),
            ('i64 255, ptr %12', 'i64 0, ptr %12'),
            ('i64 %8, ptr null, ptr null', 'i64 %8, ptr %0, ptr null'),
            ('[ %14, %6 ]', '[ %1, %6 ]'),
            ('store atomic i64 %14', 'store atomic i64 %1'),
            ('ret ptr %5', 'ret ptr %0'),
            ('@neverd_publisher_type_probe(ptr %0, ptr %0, ptr %1)',
             '@neverd_publisher_type_probe(ptr %0, ptr %1, ptr %1)'),
            ('@neverd_publisher_type_probe(ptr %0, ptr %0, ptr %1)',
             '@neverd_publisher_type_probe(ptr %0, ptr %0, ptr null)'),
            ('ret ptr %0', 'ret ptr null'),
            ('__swift_instantiateConcreteTypeFromMangledNameAbstract(ptr nonnull @"' +
             PUBLISHER_RECORD + '")',
             '__swift_instantiateConcreteTypeFromMangledNameAbstract(ptr nonnull @"other")'),
            ('ptr %2, ptr undef)', 'ptr %0, ptr undef)'),
            ('ptr %2, ptr undef)', 'ptr %2, ptr null)'),
            ('ptr nonnull @"' + PUBLISHER + '", ptr %2', 'ptr %0, ptr %2'),
            ('store atomic ptr %3', 'store atomic ptr %0'),
            ('release, align 8', 'monotonic, align 8'),
            ('icmp eq ptr %0, null', 'icmp ne ptr %0, null'),
            ('[ %3, %cacheIsNull ]', '[ %0, %cacheIsNull ]'),
            ('ret ptr %4', 'ret ptr %0'),
            ('label %cacheIsNull, label %cont', 'label %cont, label %cacheIsNull'),
            ('br label %cont', 'br label %entry'),
            ('ptr %2, ptr undef) #6', 'ptr %2, ptr undef) #6\n  call void @escape()'),
        ]
        for before, after in changes:
            with self.subTest(before=before):
                self.assertIn(before, PUBLISHER_IR)
                self.assertEqual(generic_conformance_storage(
                    PUBLISHER_IR.replace(before, after), PUBLISHER_PROBES), set())
        declaration = '@"' + PUBLISHER + '" = external global '
        declaration += '%swift.protocol_conformance_descriptor, align 4\n'
        self.assertEqual(generic_conformance_storage(
            declaration + PUBLISHER_IR, PUBLISHER_PROBES), set())
        definition = re.search(r'^define ptr @metadata_CurrentValueSubject.*?^}',
                               PUBLISHER_IR, re.M | re.S)[0]
        self.assertEqual(generic_conformance_storage(
            definition + '\n' + PUBLISHER_IR, PUBLISHER_PROBES), set())

    def test_generic_conformance_requires_complete_abis_and_bounded_input(self):
        for before, after in [
            ('declare swiftcc void @neverd_publisher_type_probe(ptr, ptr, ptr)',
             'declare swiftcc void @neverd_publisher_type_probe(ptr, ptr)'),
            ('declare ptr @swift_getWitnessTable',
             'declare swiftcc ptr @swift_getWitnessTable'),
            ('declare swiftcc ptr @swift_getTypeByMangledNameInContext2',
             'declare ptr @swift_getTypeByMangledNameInContext2'),
            ('declare swiftcc ptr @swift_getTypeByMangledNameInContextInMetadataState2',
             'declare i64 @swift_getTypeByMangledNameInContextInMetadataState2'),
        ]:
            with self.subTest(before=before), self.assertRaises(ValueError):
                generic_conformance_storage(PUBLISHER_IR.replace(before, after),
                                            PUBLISHER_PROBES)
        for invalid in ['', PUBLISHER_IR + PUBLISHER_IR,
                        ' ' * (16 * 1024 * 1024 + 1)]:
            with self.subTest(size=len(invalid)), self.assertRaises(ValueError):
                generic_conformance_storage(invalid, PUBLISHER_PROBES)
        with self.assertRaises(ValueError):
            generic_conformance_storage(PUBLISHER_IR, [('metadata.*', 'witness')])

    def test_cvararg_conformance_requires_complete_compiler_data_evidence(self):
        descriptor = '$sSSs7CVarArg10FoundationMc'
        self.assertEqual(conformance_storage(
            CVARARG, ['witness_StringCVarArg'], {NAME}, 'CVarArg'),
            {descriptor})
        for invalid in [
            CVARARG.replace('external global %swift.protocol_conformance_descriptor',
                            'external thread_local global %swift.protocol_conformance_descriptor'),
            CVARARG.replace('ptr nonnull @"' + descriptor + '"', 'ptr %descriptor'),
            CVARARG.replace('ptr nonnull @"$sSSN"', 'ptr nonnull @"$sSiN"'),
            CVARARG.replace('ptr %1) #3', 'ptr %different) #3'),
            CVARARG.replace(' release, align 8', ' monotonic, align 8'),
            CVARARG.replace('ptr undef)', 'ptr null)'),
            '@"' + descriptor + '" = external global '
            '%swift.protocol_conformance_descriptor, align 4\n' + CVARARG,
        ]:
            with self.subTest(invalid=invalid):
                self.assertEqual(conformance_storage(
                    invalid, ['witness_StringCVarArg'], {NAME}, 'CVarArg'), set())
        with self.assertRaises(ValueError):
            conformance_storage(CVARARG, ['witness_StringCVarArg'], {NAME},
                                'StringProtocol')

    def test_metadata_only_types_do_not_invent_hashable_witnesses(self):
        self.assertIn('Any', METADATA_TYPES)
        self.assertNotIn('Any', HASHABLE_TYPES)
        self.assertIn('Substring', METADATA_TYPES)
        self.assertNotIn('Substring', HASHABLE_TYPES)
        self.assertEqual(set(METADATA_TYPES) - set(HASHABLE_TYPES),
                         {'Any', 'Substring'})
        self.assertEqual(metadata_storage(ANY_IR, ['metadata_Any']), {ANY})
        for invalid in [
            ANY_IR.replace('external global', 'external thread_local global'),
            ANY_IR.replace('%swift.full_existential_type', '%swift.type'),
            ANY_IR.replace('i64 8', 'i64 0'),
            ANY_IR.replace('getelementptr inbounds', 'getelementptr'),
        ]:
            with self.subTest(invalid=invalid):
                self.assertEqual(metadata_storage(
                    invalid, ['metadata_Any']), set())

    def test_only_lazy_witness_accessors_supply_conformance_identity(self):
        self.assertEqual(conformance_storage(
            CONFORMANCE_IR, ['witness_StringProtocol'], {NAME}),
            {CONFORMANCE})
        for invalid in [
            CONFORMANCE_IR.replace('external global %swift.protocol_conformance_descriptor',
                                   'external thread_local global %swift.protocol_conformance_descriptor'),
            CONFORMANCE_IR.replace('external global %swift.protocol_conformance_descriptor',
                                   'global %swift.protocol_conformance_descriptor'),
            CONFORMANCE_IR.replace('%swift.protocol_conformance_descriptor', 'ptr'),
            CONFORMANCE_IR.replace('align 4', 'align 8'),
            CONFORMANCE_IR.replace('ptr nonnull @"$sSSSysMc"', 'ptr %descriptor'),
            CONFORMANCE_IR.replace('ptr nonnull @"$sSSN"', 'ptr nonnull @"unknown"'),
            CONFORMANCE_IR.replace('ptr undef)', 'ptr null)'),
            CONFORMANCE_IR.replace('store atomic ptr %2', 'store ptr %2'),
            CONFORMANCE_IR.replace(' release, align 8', ', align 8'),
            CONFORMANCE_IR.replace('ptr %1) #3', 'ptr %other) #3'),
            '@"$sSSSysMc" = external global '
            '%swift.protocol_conformance_descriptor, align 4\n' + CONFORMANCE_IR,
        ]:
            with self.subTest(invalid=invalid):
                self.assertEqual(conformance_storage(
                    invalid, ['witness_StringProtocol'], {NAME}), set())

    def test_substring_sequence_descriptor_requires_matching_probe(self):
        descriptor = '$sSsSTsMc'
        self.assertEqual(conformance_storage(
            SUBSTRING_SEQUENCE, ['witness_SubstringSequence'], {'$sSsN'},
            'Sequence'), {descriptor})
        self.assertEqual(conformance_storage(
            SUBSTRING_SEQUENCE, ['witness_SubstringSequence'], {NAME},
            'Sequence'), set())
        wrapped = SUBSTRING_SEQUENCE.replace(
            'define void @witness_SubstringSequence() #0 {\nentry:\n',
            'define void @witness_SubstringSequence() #0 {\nentry:\n'
            '  tail call swiftcc void @"$s4Test22substringSequenceProbeyyF"() #4\n'
            '  ret void\n}\n'
            'define swiftcc void @"$s4Test22substringSequenceProbeyyF"() #0 {\n'
            'entry:\n')
        self.assertEqual(conformance_storage(
            wrapped, ['witness_SubstringSequence'], {'$sSsN'},
            'Sequence'), {descriptor})
        self.assertEqual(conformance_storage(
            wrapped.replace('tail call swiftcc void', 'call swiftcc void'),
            ['witness_SubstringSequence'], {'$sSsN'}, 'Sequence'), set())
        with self.assertRaises(ValueError):
            conformance_storage(SUBSTRING_SEQUENCE,
                                ['witness_SubstringSequence'], {'$sSsN'},
                                'Unknown')

    def test_conformance_probe_requires_unique_definition_and_exact_abis(self):
        for invalid in [
            '', CONFORMANCE_IR + CONFORMANCE_IR,
            ' ' * (16 * 1024 * 1024 + 1),
            CONFORMANCE_IR.replace('declare ptr @swift_getWitnessTable',
                                   'declare i64 @swift_getWitnessTable'),
            CONFORMANCE_IR.replace('declare swiftcc void '
                                   '@neverd_string_protocol_probe',
                                   'declare void @neverd_string_protocol_probe'),
            CONFORMANCE_IR.replace('@witness_StringProtocol()',
                                   '@another_probe()'),
        ]:
            with self.subTest(invalid=invalid[:80]), self.assertRaises(ValueError):
                conformance_storage(invalid, ['witness_StringProtocol'], {NAME})
        with self.assertRaises(ValueError):
            conformance_storage(CONFORMANCE_IR, ['witness.*'], {NAME})

    def test_only_direct_witness_arguments_supply_storage_identity(self):
        self.assertEqual(witness_storage(WITNESS_IR, ['witness_String'], {NAME}), {WITNESS})
        for invalid in [
            WITNESS_IR.replace('external global', 'external thread_local global'),
            WITNESS_IR.replace('external global', 'global'),
            WITNESS_IR.replace('global ptr', 'global i64'),
            WITNESS_IR.replace('align 8', 'align 4'),
            WITNESS_IR.replace('ptr nonnull @"$sSSSHsWP"', 'ptr nonnull %loaded'),
            WITNESS_IR.replace('ptr nonnull @"$sSSSHsWP"',
                               'ptr nonnull getelementptr (i8, ptr @"$sSSSHsWP", i64 8)'),
            WITNESS_IR.replace('ptr nonnull @"$sSSN"', 'ptr nonnull @"unknown"'),
            WITNESS_IR.replace('call swiftcc', 'call'),
            WITNESS_IR.replace('call swiftcc', 'tail call swiftcc'),
            '@"$sSSSHsWP" = external global ptr, align 8\n' + WITNESS_IR,
        ]:
            with self.subTest(invalid=invalid):
                self.assertEqual(witness_storage(invalid, ['witness_String'], {NAME}), set())
        self.assertEqual(witness_storage(WITNESS_IR, ['witness_String'], set()), set())

    def test_witness_probe_requires_unique_definition_and_exact_generic_abi(self):
        for invalid in [
            '', WITNESS_IR + WITNESS_IR, ' ' * (16 * 1024 * 1024 + 1),
            WITNESS_IR.replace('declare swiftcc', 'declare'),
            WITNESS_IR.replace('(ptr noalias, ptr, ptr)', '(ptr noalias, ptr)'),
            WITNESS_IR.replace('@witness_String()', '@another_probe()'),
        ]:
            with self.subTest(invalid=invalid[:80]), self.assertRaises(ValueError):
                witness_storage(invalid, ['witness_String'], {NAME})
        with self.assertRaises(ValueError):
            witness_storage(WITNESS_IR, ['witness.*'], {NAME})

    def test_only_direct_non_tls_external_storage_queries_supply_facts(self):
        self.assertEqual(metadata_storage(IR, ['metadata_String']), {NAME})
        for invalid in [
            IR.replace('external global', 'external thread_local global'),
            IR.replace('external global', 'global'),
            IR.replace('%swift.type', 'ptr'),
            IR.replace('align 8', 'align 4'),
            IR.replace('ret ptr @"$sSSN"', 'ret ptr null'),
            IR.replace('ret ptr @"$sSSN"', '%x = call ptr @get()\n  ret ptr %x'),
            IR.replace('ret ptr @"$sSSN"',
                       'ret ptr getelementptr (i8, ptr @"$sSSN", i64 8)'),
            '@"$sSSN" = external global %swift.type, align 8\n' + IR,
        ]:
            with self.subTest(invalid=invalid):
                self.assertEqual(metadata_storage(invalid, ['metadata_String']), set())

    def test_missing_ambiguous_and_oversized_queries_fail(self):
        for invalid in ['', IR + IR, ' ' * (16 * 1024 * 1024 + 1)]:
            with self.subTest(size=len(invalid)), self.assertRaises(ValueError):
                metadata_storage(invalid, ['metadata_String'])
        with self.assertRaises(ValueError):
            metadata_storage(IR, ['metadata.*'])

    def test_every_compiler_and_export_profile_must_agree(self):
        profiles = [{NAME} for _ in range(4)]
        exports = [{NAME: {MODULE}} for _ in range(4)]
        self.assertIn('{"' + NAME + '",', render(profiles, exports, 'test', 'compiler'))
        for index in range(4):
            changed = copy.deepcopy(profiles)
            changed[index] = set()
            self.assertNotIn('{"' + NAME + '",',
                             render(changed, exports, 'test', 'compiler'))
            changed = copy.deepcopy(exports)
            changed[index][NAME] = {'/tmp/impostor.dylib'}
            self.assertNotIn('{"' + NAME + '",',
                             render(profiles, changed, 'test', 'compiler'))
        with self.assertRaises(ValueError):
            render(profiles[:3], exports, 'test', 'compiler')
        with self.assertRaises(ValueError):
            render(profiles, exports[:3], 'test', 'compiler')


if __name__ == '__main__':
    unittest.main()
