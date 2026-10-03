from pathlib import Path
import unittest
from scripts.generate_swift_consumed_inputs import (
    consumed_uint_input, render, RUNTIME, METADATA, WITNESS, PROVIDER)

IR = (Path(__file__).parent / 'fixtures/swift_consumed_uint.ll').read_text()
FACT = (RUNTIME, METADATA, WITNESS, 8)

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
        exports = [{n: {PROVIDER} for n in FACT[:3]} for _ in range(4)]
        self.assertIn(METADATA, render([FACT]*4, exports, '15.5', 'test'))
        for index in range(4):
            profiles=[FACT]*4; profiles[index]=(*FACT[:3],16)
            with self.assertRaises(ValueError): render(profiles,exports,'15.5','test')
            for name in FACT[:3]:
                bad=[{n:set(v) for n,v in e.items()} for e in exports]
                bad[index][name]={'wrong'}
                with self.assertRaises(ValueError): render([FACT]*4,bad,'15.5','test')
        for count in [0,1,3,5]:
            with self.assertRaises(ValueError): render([FACT]*count,exports,'15.5','test')

if __name__ == '__main__': unittest.main()
