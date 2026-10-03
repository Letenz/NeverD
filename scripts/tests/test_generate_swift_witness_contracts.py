from pathlib import Path
import unittest

from scripts.generate_swift_witness_contracts import render, unused_instantiation_argument

IR = (Path(__file__).parent / 'fixtures/swift_generic_witness.ll').read_text()
NAME = '$s7Combine19CurrentValueSubjectCyxq_GAA9PublisherAAMc'
MODULE = '/System/Library/Frameworks/Combine.framework/Combine'


class SwiftWitnessContracts(unittest.TestCase):
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
