import json
import unittest

from scripts.generate_objc_format_declarations import format_contract, render


class ObjCFormatDeclarationTests(unittest.TestCase):
    def test_compiler_attribute_positions_include_hidden_arguments(self):
        self.assertEqual(format_contract('__attribute__((format(NSString, 1, 2)))'), (2, 3))
        self.assertEqual(format_contract('__attribute__((format(NSString, 1, 3)))'), (2, 4))
        for value in ('__attribute__((annotate("format(NSString, 1, 2)")))',
                      'format(printf, 1, 2)', 'format(NSString, 1, 0)',
                      'format(NSString, 0, 2)', 'format(NSString, 3, 2)',
                      'format(NSString, 1, 100)',
                      'format(NSString, 1, 2) format(NSString, 1, 2)'):
            self.assertIsNone(format_contract(value))

    def test_all_alternatives_and_platforms_must_agree(self):
        fact = json.dumps(['@24@0:8@16', 2, 3])
        good = {'format:': {fact}}
        self.assertIn('{"format:",', render([good] * 4, 'test', 'test'))
        for wrong in ({}, {'format:': {''}}, {'format:': {'', fact}},
                      {'format:': {json.dumps(['@32@0:8@16', 2, 4])}}):
            for position in range(4):
                profiles = [good] * 4
                profiles[position] = wrong
                self.assertNotIn('{"format:",', render(profiles, 'test', 'test'))


if __name__ == '__main__':
    unittest.main()
