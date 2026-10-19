#!/usr/bin/env python3
"""Focused, offline regression tests for syscall table selection."""
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import generate_linux_syscall_arguments as gen


class SyscallGenerationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def test_native_table_precedes_generic(self):
        self.write('scripts/syscall.tbl', '1 common wrong sys_wrong\n')
        self.write('arch/arm64/tools/syscall_64.tbl',
                   '63 common read sys_read\n403 32 time64 sys_clock_gettime\n'
                   '38 renameat renameat sys_renameat\n')
        self.assertEqual(gen.generic_table(self.root),
                         [(63, 'read', 'sys_read'),
                          (38, 'renameat', 'sys_renameat')])

    def test_generic_table_filters_other_abis(self):
        self.write('scripts/syscall.tbl',
                   '1 64 write sys_write\n2 time32 bad sys_bad\n'
                   '3 32 bad sys_bad\n4 riscv bad sys_bad\n')
        self.assertEqual(gen.generic_table(self.root), [(1, 'write', 'sys_write')])

    def test_legacy_preprocessor_handles_continuations_and_guards(self):
        self.write('arch/arm64/include/uapi/asm/unistd.h',
                   '#define __BITS_PER_LONG 64\n'
                   '#include <asm-generic/unistd.h>\n')
        self.write('include/uapi/asm-generic/unistd.h',
                   '#define __NR_good 137\n'
                   '__SYSCALL(__NR_good, \\\n sys_rt_sigtimedwait)\n'
                   '#if defined(__SYSCALL_COMPAT) || __BITS_PER_LONG == 32\n'
                   '__SYSCALL(403, sys_clock_gettime)\n#endif\n')
        self.assertEqual(gen.generic_table(self.root),
                         [(137, 'rt_sigtimedwait', 'sys_rt_sigtimedwait')])

    def test_preprocessor_failure_is_not_empty_success(self):
        with self.assertRaises(FileNotFoundError):
            gen.generic_table(self.root, cpp='/nonexistent/neverd-cpp')

    def test_arm_pair_alignment_and_pointer(self):
        self.assertEqual(gen.registers(['int', 'char *', 'size_t', 'loff_t'], True), 6)
        self.assertEqual(gen.registers(['int', 'u64 *', 'int'], True), 3)
        self.assertEqual(gen.registers(['int', 'loff_t'], False), 2)

    def test_obsolete_stub_does_not_claim_zero_arguments(self):
        self.write('Makefile', 'VERSION = 7\nPATCHLEVEL = 2\n')
        self.write('include/linux/syscalls.h',
                   'asmlinkage long sys_ni_syscall(void);\n')
        for path in ('arch/x86/entry/syscalls/syscall_64.tbl',
                     'arch/arm/tools/syscall.tbl',
                     'arch/arm64/tools/syscall_64.tbl'):
            self.write(path, '18 common obsolete sys_ni_syscall\n')
        output = self.root / 'out.def'
        with patch('sys.argv', ['generator', '--kernel', str(self.root),
                               '--output', str(output)]):
            gen.main()
        self.assertNotIn('ARGUMENTS(AArch64, 18', output.read_text())

    def test_multiline_prototype(self):
        self.assertEqual(gen.split_parameters('int x,\n const char __user *p'),
                         ['int x', 'const char __user *p'])


if __name__ == '__main__':
    unittest.main()
