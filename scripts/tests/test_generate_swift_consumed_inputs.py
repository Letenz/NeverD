from pathlib import Path
import unittest
from scripts.generate_swift_consumed_inputs import (
    consumed_uint_input, consumed_identifier_input, render, RUNTIME, METADATA, WITNESS, PROVIDER,
    IDENTIFIER_METADATA, IDENTIFIER_WITNESS)

IR = (Path(__file__).parent / 'fixtures/swift_consumed_uint.ll').read_text()
FACT = (RUNTIME, METADATA, WITNESS, 8)
IDENTIFIER_FACT = (RUNTIME, IDENTIFIER_METADATA, IDENTIFIER_WITNESS, 8)
PROFILE = (FACT, IDENTIFIER_FACT)
IDENTIFIER_IR = (Path(__file__).parent / 'fixtures/swift_consumed_identifier.ll').read_text()

class SwiftConsumedInputs(unittest.TestCase):
    def test_complete_lifetime_and_names(self):
        self.assertEqual(consumed_uint_input(IR), FACT)
        renamed = IR.replace('%0', '%result').replace('%1', '%input').replace('%2', '%temporary').replace('entry:', 'first:')
        self.assertEqual(consumed_uint_input(renamed), FACT)
        self.assertEqual(consumed_uint_input(IR.replace(' #0', ' #17')), FACT)

    def test_partial_unrelated_escaping_or_reused_storage_rejects(self):
        changes = [
          ('define swiftcc void', 'define void'),
          ('i64 %1)', 'i32 %1)'),
          ('i64 %1)', 'i64 %1, ptr %other)'),
          ('%TSu = type <{ i64 }>', '%TSu = type <{ i32 }>'),
          ('%TSu = type <{ i64 }>', '%TSu = type <{ ptr }>'),
          ('alloca %TSu', 'alloca %Other'),
          ('alloca %TSu, align 8', 'alloca %TSu, align 4'),
          ('store i64 %1', 'store i32 %1'),
          ('store i64 %1', 'store i64 %other'),
          ('store i64 %1', 'store i64 0'),
          ('store i64 %1', 'store volatile i64 %1'),
          ('ptr %2, align 8', 'ptr %other, align 8'),
          ('ptr noalias nonnull %2', 'ptr noalias nonnull %other'),
          ('ptr noalias nonnull %2', 'ptr noalias nonnull %0'),
          ('ptr noalias nonnull %2', 'ptr noalias nonnull null'),
          ('ptr nonnull @"$sSuN"', 'ptr nonnull @"$sSiN"'),
          ('ptr nonnull @"$sSuSHsWP"', 'ptr nonnull @"$sSiSHsWP"'),
          ('ptr nonnull @"$sSuN"', 'ptr nonnull %metadata'),
          ('sret(%Ts11AnyHashableV) %0, ptr noalias', 'sret(%Ts11AnyHashableV) %other, ptr noalias'),
          ('sret(%Ts11AnyHashableV) %0', 'sret(%Ts11AnyHashableV) %2'),
          ('lifetime.start.p0(i64 8', 'lifetime.start.p0(i64 4'),
          ('lifetime.end.p0(i64 8', 'lifetime.end.p0(i64 4'),
          ('lifetime.end.p0(i64 8, ptr nonnull %2)', 'lifetime.end.p0(i64 8, ptr nonnull %other)'),
          ('ret void', '%read = load i64, ptr %2\n  ret void'),
          ('ret void', 'store ptr %2, ptr @escaped\n  ret void'),
          ('ret void', 'call void @escape(ptr %2)\n  ret void'),
          ('ret void', 'unreachable'),
          ('entry:', 'entry:\n  br label %other\nother:'),
          ('external global %swift.type, align 8', 'external thread_local global %swift.type, align 8'),
          ('external global ptr, align 8', 'extern_weak global ptr, align 8'),
          ('external global ptr, align 8', 'internal global ptr null, align 8'),
          ('ptr noalias, ptr, ptr)', 'ptr noalias nocapture, ptr, ptr)'),
        ]
        for before, after in changes:
            with self.subTest(after=after):
                self.assertIn(before, IR)
                with self.assertRaises(ValueError):
                    consumed_uint_input(IR.replace(before, after))
        for line in [
            '  call void @llvm.lifetime.start.p0(i64 8, ptr nonnull %2)\n',
            '  call void @llvm.lifetime.end.p0(i64 8, ptr nonnull %2)\n',
            '  store i64 %1, ptr %2, align 8\n',
        ]:
            with self.subTest(line=line):
                self.assertIn(line, IR)
                with self.assertRaises(ValueError): consumed_uint_input(IR.replace(line, ''))
        for suffix in [IR, '\n%TSu = type <{ i64 }>\n',
                       '\n@"$sSuN" = external global %swift.type, align 8\n',
                       '\n@"$sSuSHsWP" = external global ptr, align 8\n']:
            with self.assertRaises(ValueError): consumed_uint_input(IR + suffix)
        with self.assertRaises(ValueError): consumed_uint_input(' ' * (1024*1024+1))

    def test_all_targets_and_independent_exports(self):
        exports = [{n: {PROVIDER} for f in PROFILE for n in f[:3]} for _ in range(4)]
        self.assertIn(METADATA, render([PROFILE]*4, exports, '15.5', 'test'))
        for index in range(4):
            profiles=[PROFILE]*4; profiles[index]=((*FACT[:3],16),IDENTIFIER_FACT)
            with self.assertRaises(ValueError): render(profiles,exports,'15.5','test')
            for name in {n for f in PROFILE for n in f[:3]}:
                bad=[{n:set(v) for n,v in e.items()} for e in exports]
                bad[index][name]={'wrong'}
                with self.assertRaises(ValueError): render([PROFILE]*4,bad,'15.5','test')
        for count in [0,1,3,5]:
            with self.assertRaises(ValueError): render([PROFILE]*count,exports,'15.5','test')

    def test_identifier_has_independent_complete_typed_lifetime(self):
        self.assertEqual(consumed_identifier_input(IDENTIFIER_IR), IDENTIFIER_FACT)
        self.assertEqual(consumed_uint_input(IDENTIFIER_IR), FACT)
        renamed = IDENTIFIER_IR.replace('%0', '%result').replace('%1', '%input').replace('%2', '%temporary').replace('entry:', 'start:')
        self.assertEqual(consumed_identifier_input(renamed), IDENTIFIER_FACT)
        with self.assertRaises(ValueError): consumed_identifier_input(IR)
        for before, after in [
            ('ptr %1)', 'i64 %1)'),
            ('%TSO = type <{ ptr }>', '%TSO = type <{ i64 }>'),
            ('%TSO = type <{ ptr }>', '%TSO = type <{ ptr, ptr }>'),
            ('alloca %TSO', 'alloca %TSu'),
            ('store ptr %1', 'store i64 %1'),
            ('store ptr %1', 'store i32 %1'),
            ('store ptr %1', 'store ptr null'),
            ('store ptr %1', 'store ptr %0'),
            ('ptr nonnull @"$sSON"', 'ptr nonnull @"$sSuN"'),
            ('ptr nonnull @"$sSOSHsWP"', 'ptr nonnull @"$sSuSHsWP"'),
            ('ptr nonnull @"$sSOSHsWP"', 'ptr nonnull %witness'),
            ('ptr noalias nonnull %2', 'ptr noalias nonnull %0'),
            ('lifetime.start.p0(i64 8', 'lifetime.start.p0(i64 4'),
            ('lifetime.end.p0(i64 8', 'lifetime.end.p0(i64 16'),
            ('lifetime.end.p0(i64 8, ptr nonnull %2)', 'lifetime.end.p0(i64 8, ptr nonnull %0)'),
            ('store ptr %1, ptr %2, align 8', 'store ptr %1, ptr %other, align 8'),
            ('store ptr %1, ptr %2, align 8', ''),
            ('external global %swift.type, align 8', 'extern_weak global %swift.type, align 8'),
            ('external global ptr, align 8', 'external thread_local global ptr, align 8'),
            ('ret void', '%read = load ptr, ptr %2\n  ret void'),
            ('ret void', 'store ptr %2, ptr @escaped\n  ret void'),
            ('ret void', 'call void @escape(ptr %2)\n  ret void'),
            ('ptr noalias, ptr, ptr)', 'ptr noalias nocapture, ptr, ptr)'),
        ]:
            with self.subTest(after=after):
                self.assertIn(before, IDENTIFIER_IR)
                with self.assertRaises(ValueError): consumed_identifier_input(IDENTIFIER_IR.replace(before, after))
        for suffix in [IDENTIFIER_IR, '\n%TSO = type <{ ptr }>\n',
                       '\n@"$sSON" = external global %swift.type, align 8\n',
                       '\n@"$sSOSHsWP" = external global ptr, align 8\n']:
            with self.assertRaises(ValueError): consumed_identifier_input(IDENTIFIER_IR + suffix)

    def test_equal_extent_does_not_merge_different_contracts(self):
        exports = [{n: {PROVIDER} for f in PROFILE for n in f[:3]} for _ in range(4)]
        for index in range(4):
            for candidate in [(FACT, FACT), (IDENTIFIER_FACT, FACT),
                              (FACT, (RUNTIME, METADATA, IDENTIFIER_WITNESS, 8)),
                              (FACT, (RUNTIME, IDENTIFIER_METADATA, WITNESS, 8)),
                              (FACT, (*IDENTIFIER_FACT[:3],16)), (FACT,)]:
                profiles = [PROFILE]*4; profiles[index] = candidate
                with self.subTest(index=index, candidate=candidate):
                    with self.assertRaises(ValueError): render(profiles, exports, '15.5', 'test')

if __name__ == '__main__': unittest.main()
