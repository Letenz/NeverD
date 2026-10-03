from pathlib import Path
import re
import unittest

from scripts.generate_swift_opaque_values import (
    opaque_value_contract, render, METADATA, EQUAL, PROVIDER)

IR = (Path(__file__).parent / 'fixtures/swift_opaque_value.ll').read_text()
FACT = (METADATA, EQUAL, 40, 16, 8)


class SwiftOpaqueValues(unittest.TestCase):
    def test_complete_lifetime_allows_only_local_renaming(self):
        self.assertEqual(opaque_value_contract(IR), FACT)
        renamed = re.sub(r'%([0-9]+)\b', r'%value\1', IR)
        renamed = renamed.replace('%InitializeWithCopy', '%copy_operation')
        renamed = renamed.replace('%Destroy', '%destroy_operation').replace('entry:', 'first:')
        self.assertEqual(opaque_value_contract(renamed), FACT)
        self.assertEqual(opaque_value_contract(IR.replace(' #2', ' #27')), FACT)

    def test_witness_lifetime_and_read_identity_are_independent(self):
        changes = [
            ('external global %swift.type, align 8', 'extern_weak global %swift.type, align 8'),
            ('external global %swift.type, align 8', 'external thread_local global %swift.type, align 8'),
            ('external global %swift.type, align 8', 'external global %swift.type, align 4'),
            ('type { [24 x i8], ptr, ptr }', 'type { [16 x i8], ptr, ptr }'),
            ('type <{ %Ts15_AnyHashableBoxP }>', 'type <{ %Other }>'),
            ('linkonce_odr hidden ptr', 'linkonce_odr hidden i64'),
            ('i64 -8', 'i64 -16'),
            ('i64 16', 'i64 24'),
            ('i64 8\n  %Destroy', 'i64 16\n  %Destroy'),
            ('load ptr, ptr %2, align 8', 'load ptr, ptr %0, align 8'),
            ('ptr noalias %1, ptr noalias %0', 'ptr noalias %0, ptr noalias %1'),
            ('ptr noalias %1, ptr noalias %0', 'ptr noalias %1, ptr noalias %1'),
            ('ptr nonnull @"$ss11AnyHashableVN") #4', 'ptr nonnull %other) #4'),
            ('ret ptr %1', 'ret ptr %3'),
            ('ret ptr %0', 'ret ptr null'),
            ('!18 = !{i64 88}', '!18 = !{i64 80}'),
            ('!17 = !{}', '!17 = !{i64 1}'),
            ('attributes #4 = { nounwind }', 'attributes #4 = { nounwind noreturn }'),
            ('alloca %Ts11AnyHashableV, align 8', 'alloca %Ts11AnyHashableV, align 4'),
            ('alloca %Ts11AnyHashableV', 'alloca i64'),
            ('lifetime.start.p0(i64 40', 'lifetime.start.p0(i64 32'),
            ('lifetime.end.p0(i64 40', 'lifetime.end.p0(i64 32'),
            ('lifetime.end.p0(i64 40, ptr nonnull %3)', 'lifetime.end.p0(i64 40, ptr nonnull %0)'),
            ('(ptr nonnull %0, ptr nonnull %3)', '(ptr nonnull %3, ptr nonnull %0)'),
            ('(ptr nonnull %0, ptr nonnull %3)', '(ptr nonnull %0, ptr nonnull %0)'),
            ('ptr noalias nocapture nonnull dereferenceable(40) %3', 'ptr noalias nonnull dereferenceable(40) %3'),
            ('ptr noalias nocapture nonnull dereferenceable(40) %3', 'ptr noalias nocapture nonnull dereferenceable(40) %0'),
            ('%6 = call ptr @"$ss11AnyHashableVWOh"(ptr nonnull %3)', '%6 = call ptr @"$ss11AnyHashableVWOh"(ptr nonnull %0)'),
            ('  ret i1 %5', '  %later = load i64, ptr %3\n  ret i1 %5'),
            ('  ret i1 %5', '  store ptr %3, ptr @escaped\n  ret i1 %5'),
            ('  ret i1 %5', '  call void @escape(ptr %3)\n  ret i1 %5'),
            ('  ret i1 %5', '  ret i1 %other'),
            ('  ret i1 %5', '  unreachable'),
            ('entry:', 'entry:\n  br label %second\nsecond:'),
            ('declare swiftcc i1', 'declare swiftcc i8'),
            ('declare swiftcc i1', 'declare i1'),
            ('dereferenceable(40)', 'dereferenceable(32)'),
            ('ptr noalias nocapture dereferenceable(40)) local_unnamed_addr', 'ptr noalias dereferenceable(40)) local_unnamed_addr'),
            ('ret i1 %2', 'ret i1 true'),
            ('dereferenceable(40) %1)\n  ret i1 %2', 'dereferenceable(40) %0)\n  ret i1 %2'),
        ]
        for before, after in changes:
            with self.subTest(after=after):
                self.assertIn(before, IR)
                with self.assertRaises(ValueError): opaque_value_contract(IR.replace(before, after))
        for line in [
            '  call void @llvm.lifetime.start.p0(i64 40, ptr nonnull %3)\n',
            '  %4 = call ptr @"$ss11AnyHashableVWOc"(ptr nonnull %0, ptr nonnull %3)\n',
            '  %6 = call ptr @"$ss11AnyHashableVWOh"(ptr nonnull %3)\n',
            '  call void @llvm.lifetime.end.p0(i64 40, ptr nonnull %3)\n',
        ]:
            self.assertIn(line, IR)
            with self.assertRaises(ValueError): opaque_value_contract(IR.replace(line, ''))
        for suffix in [IR, '\n@"$ss11AnyHashableVN" = external global %swift.type, align 8\n',
                       '\n%Ts15_AnyHashableBoxP = type { [24 x i8], ptr, ptr }\n', '\n!18 = !{i64 88}\n']:
            with self.assertRaises(ValueError): opaque_value_contract(IR + suffix)
        with self.assertRaises(ValueError): opaque_value_contract(' ' * (1024 * 1024 + 1))

    def test_all_profiles_and_exact_exports(self):
        exports = [{n: {PROVIDER} for n in FACT[:2]} for _ in range(4)]
        self.assertIn(METADATA, render([FACT] * 4, exports, '15.5', 'test'))
        for index in range(4):
            profiles = [FACT] * 4
            profiles[index] = (*FACT[:2], 32, 16, 8)
            with self.assertRaises(ValueError): render(profiles, exports, '15.5', 'test')
            for name in FACT[:2]:
                bad = [{n: set(v) for n, v in e.items()} for e in exports]
                bad[index][name] = {'wrong'}
                with self.assertRaises(ValueError): render([FACT] * 4, bad, '15.5', 'test')
        for count in [0, 1, 3, 5]:
            with self.assertRaises(ValueError): render([FACT] * count, exports, '15.5', 'test')


if __name__ == '__main__': unittest.main()
