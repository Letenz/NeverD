from pathlib import Path
import re
import unittest

from scripts.generate_swift_witness_contracts import (
    render, unused_instantiation_argument, string_protocol_instantiation_argument,
    main_actor_instantiation_argument)
from scripts.generate_swift_data_declarations import generic_range_conformance_storage

IR = (Path(__file__).parent / 'fixtures/swift_generic_witness.ll').read_text()
NAME = '$s7Combine19CurrentValueSubjectCyxq_GAA9PublisherAAMc'
MODULE = '/System/Library/Frameworks/Combine.framework/Combine'
RANGE_IR = (Path(__file__).parent / 'fixtures/swift_generic_range_witness.ll').read_text()
RANGE = '$sSnyxGSXsMc'
CORE = '/usr/lib/swift/libswiftCore.dylib'
STRING_IR = (Path(__file__).parent / 'fixtures/swift_string_protocol_witness.ll').read_text()
STRING = '$sSSSysMc'
ACTOR_IR = (Path(__file__).parent / 'fixtures/swift_main_actor_witness.ll').read_text()
ACTOR = '$sScMScAsMc'
CONCURRENCY = '/usr/lib/swift/libswift_Concurrency.dylib'


class SwiftWitnessContracts(unittest.TestCase):
    def test_fixed_main_actor_requires_static_flow_and_all_paired_exports(self):
        self.assertEqual(main_actor_instantiation_argument(ACTOR_IR), ACTOR)
        profiles = [{NAME, RANGE, STRING, ACTOR}] * 4
        exports = [{NAME: {MODULE}, RANGE: {CORE}, STRING: {CORE},
                    **{name: {CONCURRENCY} for name in
                       (ACTOR, '$sScMScAsWP', '$sScMMa')}} for _ in range(4)]
        self.assertIn('"' + ACTOR + '"', render(profiles, exports, '15.5', 'Swift 6.1.2'))
        for index in range(4):
            for name in (ACTOR, '$sScMScAsWP', '$sScMMa'):
                changed = [dict(export) for export in exports]
                changed[index].pop(name)
                with self.subTest(index=index, name=name), self.assertRaises(ValueError):
                    render(profiles, changed, '15.5', 'Swift 6.1.2')
        with self.assertRaises(ValueError):
            main_actor_instantiation_argument(ACTOR_IR.replace('ptr nonnull @"$sScMScAsWP"', 'ptr undef'))

    def test_string_protocol_keeps_complete_cache_query(self):
        self.assertEqual(string_protocol_instantiation_argument(STRING_IR), STRING)
        renamed = re.sub(r'%([0-9]+)', r'%local_\1', STRING_IR)
        renamed = renamed.replace('entry', 'first').replace('cacheIsNull', 'miss').replace('cont', 'join')
        self.assertEqual(string_protocol_instantiation_argument(renamed), STRING)
        profiles = [{NAME, RANGE, STRING}] * 4
        exports = [{NAME: {MODULE}, RANGE: {CORE}, STRING: {CORE}}] * 4
        self.assertIn('"' + STRING + '"', render(profiles, exports, '15.5', 'Swift 6.1.2'))

    def test_string_protocol_rejects_stale_cache_abi_and_flow(self):
        changes = [
            ('define swiftcc void', 'define void'),
            ('@neverd_string_protocol_witness_probe()', '@neverd_string_protocol_witness_probe(ptr %context)'),
            ('linkonce_odr hidden ptr', 'linkonce_odr hidden swiftcc ptr'),
            ('@"$sS2SSysWl"()', '@"$sS2SSysWl"(ptr %context)'),
            ('global ptr null', 'global ptr undef'),
            ('global ptr null', 'constant ptr null'),
            ('external global %swift.type, align 8', 'external thread_local global %swift.type, align 8'),
            ('external global %swift.protocol_conformance_descriptor', 'extern_weak global %swift.protocol_conformance_descriptor'),
            ('external global %swift.protocol_conformance_descriptor', 'external global ptr'),
            ('load ptr, ptr', 'load volatile ptr, ptr'),
            ('load ptr, ptr @"$sS2SSysWL"', 'load ptr, ptr @"foreign_cache"'),
            ('icmp eq ptr %0, null', 'icmp ne ptr %0, null'),
            ('label %cacheIsNull, label %cont', 'label %cont, label %cacheIsNull'),
            ('ptr undef)', 'ptr null)'),
            ('ptr undef)', 'ptr poison)'),
            ('ptr undef)', 'ptr %context)'),
            ('ptr undef)', 'i64 undef)'),
            ('ptr nonnull @"$sSSN", ptr undef', 'ptr nonnull @"OtherMetadata", ptr undef'),
            ('ptr nonnull @"$sSSSysMc", ptr nonnull', 'ptr nonnull @"OtherDescriptor", ptr nonnull'),
            ('store atomic ptr %2', 'store atomic ptr %0'),
            ('release, align 8', 'monotonic, align 8'),
            ('phi ptr [ %0, %entry ], [ %2, %cacheIsNull ]', 'phi ptr [ %2, %entry ], [ %0, %cacheIsNull ]'),
            ('ret ptr %3', 'ret ptr %0'),
            ('ptr %0)\n  ret void', 'ptr undef)\n  ret void'),
            ('declare ptr @swift_getWitnessTable(ptr, ptr, ptr)', 'declare swiftcc ptr @swift_getWitnessTable(ptr, ptr, ptr)'),
            ('declare swiftcc void @neverd_string_protocol_observer(ptr, ptr, ptr)', 'declare swiftcc void @neverd_string_protocol_observer(ptr, ptr)'),
            ('  ret void', '  call void @extra_effect()\n  ret void'),
            ('  ret ptr %3', '  call void @extra_effect()\n  ret ptr %3'),
        ]
        for old, new in changes:
            with self.subTest(old=old, new=new):
                self.assertIn(old, STRING_IR)
                with self.assertRaises(ValueError):
                    string_protocol_instantiation_argument(STRING_IR.replace(old, new))
        for prefix in ('@"$sSSN" =', '@"$sSSSysMc" =', '@"$sS2SSysWL" =',
                       'declare ptr @swift_getWitnessTable',
                       'declare swiftcc void @neverd_string_protocol_observer',
                       'define swiftcc void @neverd_string_protocol_witness_probe',
                       'define linkonce_odr hidden ptr @"$sS2SSysWl"'):
            line = next(line for line in STRING_IR.splitlines() if line.startswith(prefix))
            with self.subTest(duplicate=prefix), self.assertRaises(ValueError):
                string_protocol_instantiation_argument(STRING_IR + '\n' + line + '\n')
        with self.assertRaises(ValueError):
            string_protocol_instantiation_argument(' ' * (1024 * 1024 + 1))

    def test_generic_range_preserves_both_metadata_inputs(self):
        self.assertEqual(generic_range_conformance_storage(RANGE_IR), {RANGE})
        renamed = re.sub(r'%([A-Za-z_0-9.]+)', r'%renamed_\1', RANGE_IR)
        # Type identities are declarations, not SSA values.
        renamed = renamed.replace('%renamed_swift.', '%swift.')
        renamed = renamed.replace('entry:', 'first:').replace(' #2', ' #9')
        self.assertEqual(generic_range_conformance_storage(renamed), {RANGE})
        with self.assertRaises(ValueError):
            unused_instantiation_argument(RANGE_IR)
        with self.assertRaises(ValueError):
            generic_range_conformance_storage(IR)

    def test_generic_range_rejects_changed_inputs_and_complete_flow(self):
        changes = [
            ('define swiftcc void', 'define void'),
            ('ptr %Bound, ptr %Bound.Comparable) #0', 'ptr %Bound) #0'),
            ('ptr %Bound, ptr %Bound.Comparable) #0', 'ptr %Bound, ptr %Bound) #0'),
            ('ptr %Bound, ptr %Bound.Comparable) #0', 'ptr %Bound, ptr %Bound.Comparable, ptr %extra) #0'),
            ('type { ptr, i64 }', 'type { i64, ptr }'),
            ('type { ptr, i64 }', 'type { ptr, i32 }'),
            ('external global %swift.protocol_conformance_descriptor', 'external thread_local global %swift.protocol_conformance_descriptor'),
            ('external global %swift.protocol_conformance_descriptor', 'extern_weak global %swift.protocol_conformance_descriptor'),
            ('external global %swift.protocol_conformance_descriptor', 'external global ptr'),
            ('align 4', 'align 8'),
            ('"$sSnMa"', '"$sOtherMa"'),
            ('"$sSnyxGSXsMc"', '"$sOtherMc"'),
            ('(i64 0, ptr %Bound, ptr %Bound.Comparable)', '(i64 255, ptr %Bound, ptr %Bound.Comparable)'),
            ('(i64 0, ptr %Bound, ptr %Bound.Comparable)', '(i64 0, ptr %Bound.Comparable, ptr %Bound)'),
            ('(i64 0, ptr %Bound, ptr %Bound.Comparable)', '(i64 0, ptr %Bound, ptr %other)'),
            ('(i64 0, ptr %Bound, ptr %Bound.Comparable)', '(i64 0, ptr %other, ptr %Bound.Comparable)'),
            ('(i64 0, ptr %Bound, ptr %Bound.Comparable)', '(i64 0, ptr %Bound, ptr null)'),
            ('(i64 0, ptr %Bound, ptr %Bound.Comparable)', '(i64 0, ptr @concrete, ptr %Bound.Comparable)'),
            ('extractvalue %swift.metadata_response %0, 0', 'extractvalue %swift.metadata_response %0, 1'),
            ('extractvalue %swift.metadata_response %0, 0', 'extractvalue %swift.metadata_response %other, 0'),
            ('tail call swiftcc %swift.metadata_response', 'tail call swiftcc %different_response'),
            ('extractvalue %swift.metadata_response', 'extractvalue %different_response'),
            ('ptr %1, ptr undef)', 'ptr %Bound, ptr undef)'),
            ('ptr %1, ptr undef)', 'ptr %other, ptr undef)'),
            ('ptr %1, ptr undef)', 'ptr %1, ptr null)'),
            ('ptr %1, ptr undef)', 'ptr %1, ptr poison)'),
            ('ptr %1, ptr undef)', 'ptr %1, ptr %Bound)'),
            ('ptr %1, ptr undef)', 'ptr %1, i64 undef)'),
            ('@neverd_range_probe(ptr %1, ptr %1, ptr %2)', '@neverd_range_probe(ptr %1, ptr %2, ptr %1)'),
            ('@neverd_range_probe(ptr %1, ptr %1, ptr %2)', '@neverd_range_probe(ptr %1, ptr %1, ptr undef)'),
            ('@neverd_range_probe(ptr %1, ptr %1, ptr %2)', '@neverd_range_probe(ptr %1, ptr %other, ptr %2)'),
            ('tail call ptr @swift_getWitnessTable', 'tail call ptr %callee'),
            ('tail call ptr @swift_getWitnessTable', 'tail call swiftcc ptr @swift_getWitnessTable'),
            ('declare ptr @swift_getWitnessTable(ptr, ptr, ptr)', 'declare ptr @swift_getWitnessTable(ptr, ptr)'),
            ('declare swiftcc void @neverd_range_probe(ptr, ptr, ptr)', 'declare swiftcc void @neverd_range_probe(ptr, ptr)'),
            ('@"$sSnMa"(i64, ptr, ptr)', '@"$sSnMa"(i64, ptr)'),
            ('declare swiftcc %swift.metadata_response', 'declare %swift.metadata_response'),
            ('ret void', 'call void @escape(ptr %1)\n  ret void'),
            ('ret void', 'unreachable'),
            ('entry:', 'entry:\n  br label %again\nagain:'),
        ]
        for before, after in changes:
            with self.subTest(after=after):
                self.assertIn(before, RANGE_IR)
                with self.assertRaises(ValueError):
                    generic_range_conformance_storage(RANGE_IR.replace(before, after))
        for suffix in [
            '\ndefine void @neverd_generic_range_probe() {\nret void\n}\n',
            '\ndeclare ptr @swift_getWitnessTable(ptr, ptr, ptr)\n',
            '\n@"' + RANGE + '" = external global ptr\n',
            '\n%swift.metadata_response = type { ptr, i64 }\n',
            RANGE_IR,
        ]:
            with self.subTest(suffix=suffix[:50]), self.assertRaises(ValueError):
                generic_range_conformance_storage(RANGE_IR + suffix)
        renamed_type = RANGE_IR.replace(
            'tail call swiftcc %swift.metadata_response',
            'tail call swiftcc %different_response').replace(
            'extractvalue %swift.metadata_response',
            'extractvalue %different_response')
        with self.assertRaises(ValueError):
            generic_range_conformance_storage(renamed_type)
        with self.assertRaises(ValueError):
            generic_range_conformance_storage(' ' * (1024 * 1024 + 1))

    def test_both_contracts_require_all_profiles_and_exports(self):
        profiles = [{NAME, RANGE} for _ in range(4)]
        exports = [{NAME: {MODULE}, RANGE: {CORE}} for _ in range(4)]
        result = render(profiles, exports, '15.5', 'test')
        self.assertIn(NAME, result)
        self.assertIn(RANGE, result)
        for index in range(4):
            changed = profiles.copy()
            changed[index] = {NAME}
            with self.assertRaises(ValueError):
                render(changed, exports, '15.5', 'test')
            changed_exports = exports.copy()
            changed_exports[index] = {NAME: {MODULE}}
            with self.assertRaises(ValueError):
                render(profiles, changed_exports, '15.5', 'test')

    def test_generic_flow_and_ssa_renaming(self):
        self.assertEqual(unused_instantiation_argument(IR), NAME)
        renamed = IR.replace('%0', '%type').replace('%1', '%table').replace('entry:', 'first:')
        self.assertEqual(unused_instantiation_argument(renamed), NAME)
        self.assertEqual(unused_instantiation_argument(IR.replace(' #2', ' #9')), NAME)

    def test_incomplete_or_specific_instantiation_rejects(self):
        changes = [
            ('define swiftcc void @neverd_generic_publisher_probe(ptr %0)',
             'define void @neverd_generic_publisher_probe(ptr %0)'),
            ('define swiftcc void @neverd_generic_publisher_probe(ptr %0)',
             'define swiftcc void @neverd_generic_publisher_probe(ptr %0, ptr %other)'),
            ('ptr %0, ptr undef', 'ptr null, ptr undef'),
            ('ptr %0, ptr undef', 'ptr @concrete, ptr undef'),
            ('ptr %0, ptr undef', 'ptr %other, ptr undef'),
            ('ptr %0, ptr undef', 'ptr %0, ptr null'),
            ('ptr %0, ptr undef', 'ptr %0, ptr poison'),
            ('ptr %0, ptr undef', 'ptr %0, ptr %0'),
            ('ptr %0, ptr undef', 'ptr %0, i64 undef'),
            ('ptr %0, ptr %0, ptr %1', 'ptr %0, ptr %1, ptr %0'),
            ('ptr %0, ptr %0, ptr %1', 'ptr %0, ptr %other, ptr %1'),
            ('ptr %0, ptr %0, ptr %1', 'ptr %0, ptr %0, ptr undef'),
            ('ret void', 'call void @escape(ptr %0)\n  ret void'),
            ('ret void', 'unreachable'),
            ('entry:', 'entry:\n  br label %again\nagain:'),
            ('tail call ptr @swift_getWitnessTable', 'tail call swiftcc ptr @swift_getWitnessTable'),
            ('tail call ptr @swift_getWitnessTable', 'tail call ptr %callee'),
            ('declare ptr @swift_getWitnessTable(ptr, ptr, ptr)',
             'declare ptr @swift_getWitnessTable(ptr, ptr)'),
            ('declare ptr @swift_getWitnessTable(ptr, ptr, ptr)',
             'declare swiftcc ptr @swift_getWitnessTable(ptr, ptr, ptr)'),
            ('declare swiftcc void @neverd_publisher_probe(ptr, ptr, ptr)',
             'declare swiftcc void @neverd_publisher_probe(ptr, ptr)'),
            ('external global %swift.protocol_conformance_descriptor, align 4',
             'external thread_local global %swift.protocol_conformance_descriptor, align 4'),
            ('external global %swift.protocol_conformance_descriptor, align 4',
             'extern_weak global %swift.protocol_conformance_descriptor, align 4'),
            ('external global %swift.protocol_conformance_descriptor, align 4',
             'external global ptr, align 8'),
            ('external global %swift.protocol_conformance_descriptor, align 4',
             'internal global %swift.protocol_conformance_descriptor zeroinitializer, align 4'),
        ]
        for before, after in changes:
            with self.subTest(after=after):
                self.assertIn(before, IR)
                with self.assertRaises(ValueError):
                    unused_instantiation_argument(IR.replace(before, after))
        for suffix in [
            '\ndefine void @neverd_generic_publisher_probe() {\nret void\n}\n',
            '\ndeclare ptr @swift_getWitnessTable(ptr, ptr, ptr)\n',
            '\n@"' + NAME + '" = external global ptr\n',
            IR,
        ]:
            with self.subTest(suffix=suffix[:50]), self.assertRaises(ValueError):
                unused_instantiation_argument(IR + suffix)
        with self.assertRaises(ValueError):
            unused_instantiation_argument(' ' * (1024 * 1024 + 1))

    def test_four_profiles_and_exports(self):
        profiles = [NAME] * 4
        exports = [{NAME: {MODULE}} for _ in range(4)]
        self.assertIn(NAME, render(profiles, exports, '15.5', 'test'))
        for index in range(4):
            changed = profiles.copy()
            changed[index] += 'other'
            with self.assertRaises(ValueError):
                render(changed, exports, '15.5', 'test')
            changed_exports = exports.copy()
            changed_exports[index] = {}
            with self.assertRaises(ValueError):
                render(profiles, changed_exports, '15.5', 'test')
        for count in (0, 1, 3, 5):
            with self.assertRaises(ValueError):
                render([NAME] * count, exports, '15.5', 'test')


if __name__ == '__main__':
    unittest.main()
