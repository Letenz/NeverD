import unittest

from scripts.generate_swift_c_declarations import declarations, render


def record(name='swift_allocate', module='Swift', cc='C_CC',
           returns='RETURNS(RefCountedPtrTy)', args='ARGS(TypeMetadataPtrTy, SizeTy)',
           availability='AlwaysAvailable', attrs='ATTRS(NoUnwind)'):
    return (f'FUNCTION(Allocate, {module}, {name}, {cc}, {availability}, '
            f'{returns}, {args}, {attrs}, EFFECT(RuntimeEffect::Allocating), '
            'UNKNOWN_MEMEFFECTS)\n')


class SwiftCDeclarationTests(unittest.TestCase):
    def test_fixed_pointer_size_facts_are_deterministic(self):
        source = record() + record('swift_free', returns='RETURNS(VoidTy)',
                                    args='ARGS(RefCountedPtrTy, SizeTy, SizeTy)')
        facts = declarations(source)
        self.assertEqual(facts, {'swift_allocate': ('ppz', False),
                                 'swift_free': ('vpzz', False)})
        self.assertEqual(render(facts, '0' * 64),
                         render(declarations(source + source), '0' * 64))

    def test_non_c_abi_unknown_representations_and_module_cannot_bind(self):
        cases = [dict(cc='SwiftCC'), dict(cc='SwiftDirectRR_CC'),
                 dict(module='objc2'), dict(module='stdlib'),
                 dict(availability='SwiftRuntime53'), dict(args='NO_ARGS'),
                 dict(args='ARGS(Int32Ty)'), dict(args='ARGS(ObjCBoolTy)'),
                 dict(args='ARGS(UnknownPtrTy)'), dict(args='ARGS(VoidTy)'),
                 dict(args='ARGS(TypeMetadataPtrTy->getPointerTo())'),
                 dict(returns='RETURNS(RefCountedPtrTy, OpaquePtrTy)'),
                 dict(returns='RETURNS(Int1Ty)'), dict(returns='RETURNS()'),
                 dict(args='ARGS(' + ','.join(['PtrTy'] * 17) + ')')]
        for case in cases:
            with self.subTest(case=case):
                self.assertEqual(declarations(record(**case)), {})
                self.assertEqual(declarations(record() + record(**case)), {})

    def test_macro_bodies_comments_and_conditional_declarations_supply_no_facts(self):
        hidden = [f'/* {record()} */', f'// {record()}',
                  '#define WRAPPER() \\\n' + record(),
                  '#if TARGET_A\n' + record() + '#else\n' + record() + '#endif\n',
                  '#ifndef A\n#ifdef B\n' + record() + '#endif\n#endif\n']
        for source in hidden:
            with self.subTest(source=source):
                self.assertEqual(declarations(source), {})
                self.assertEqual(declarations(source + record('swift_visible')),
                                 {'swift_visible': ('ppz', False)})

    def test_conflicting_alternatives_veto_in_both_orders(self):
        changed = record(returns='RETURNS(VoidTy)')
        for source in (record() + changed, changed + record()):
            self.assertEqual(declarations(source), {})

    def test_only_explicit_void_noreturn_is_preserved(self):
        self.assertEqual(declarations(record(attrs='ATTRS(NoUnwind, NoReturn)')), {})
        self.assertEqual(declarations(record(returns='RETURNS(VoidTy)',
                                              attrs='ATTRS(NoUnwind, NoReturn)')),
                         {'swift_allocate': ('vpz', True)})

    def test_incomplete_or_exhausted_input_cannot_publish_partial_facts(self):
        for source in ('FUNCTION(', record() + '\n#if A', '#endif',
                       '#define X \\', record(args='ARGS(' * 20 + 'PtrTy' + ')' * 20),
                       record() + ' ' * (2 * 1024 * 1024), 'FUNCTION(A,B)'):
            with self.subTest(source=source[:60]), self.assertRaises(ValueError):
                declarations(source)


if __name__ == '__main__':
    unittest.main()
