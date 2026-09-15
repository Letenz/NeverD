import json
import unittest

from scripts.generate_objc_predicate_declarations import predicate_contract, render


class PredicateDeclarationTests(unittest.TestCase):
    def test_documented_entries_require_matching_compiler_types_and_owner(self):
        for owner, selector in [('NSPredicate', 'predicateWithFormat:'),
                                ('NSExpression', 'expressionWithFormat:')]:
            args = [owner, selector, True, True, ['NSString *'], owner + ' *']
            self.assertEqual(predicate_contract(*args), (2, 3))
            for index, value in [(0, 'Other'), (1, 'unrelated:'), (2, False),
                                 (3, False), (4, ['id']), (4, []),
                                 (4, ['NSString *', 'id']), (5, 'id'),
                                 (5, 'void')]:
                bad = args.copy()
                bad[index] = value
                self.assertIsNone(predicate_contract(*bad), bad)

    def test_all_alternatives_and_platforms_must_agree(self):
        fact = json.dumps(['@24@0:8@16', 2, 3])
        good = {'predicateWithFormat:': {fact}}
        rendered = render([good] * 4, 'test', 'test')
        self.assertIn('{"predicateWithFormat:",', rendered)
        self.assertIn('SourceCallTypeHint::FormatSyntax::Predicate', rendered)
        for wrong in ({}, {'predicateWithFormat:': {''}},
                      {'predicateWithFormat:': {'', fact}},
                      {'predicateWithFormat:': {json.dumps(['v24@0:8@16', 2, 3])}}):
            for index in range(4):
                profiles = [good] * 4
                profiles[index] = wrong
                self.assertNotIn('{"predicateWithFormat:",',
                                 render(profiles, 'test', 'test'))


if __name__ == '__main__':
    unittest.main()
