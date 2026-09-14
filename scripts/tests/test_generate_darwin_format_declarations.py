import json
import unittest

from scripts.generate_darwin_format_declarations import render
from scripts.generate_objc_format_declarations import format_contract


class DarwinFormatDeclarationTests(unittest.TestCase):
    def test_c_format_has_no_hidden_receiver_or_command(self):
        self.assertEqual(format_contract('__attribute__((format(NSString, 1, 2)))', 0), (0, 1))

    def test_format_declaration_requires_exports_on_both_architectures(self):
        fact = json.dumps(['v8@0', 0, 1])
        profiles = [{'log': {fact}}] * 4
        exports = [{'log': {'/usr/lib/public.dylib'}}] * 2
        self.assertIn('{"log", "v8@0", "v8@0", 0, 1,', render(profiles, exports, 'test', 'test'))
        for bad in ([{}, exports[0]], [exports[0], {}]):
            self.assertNotIn('{"log",', render(profiles, bad, 'test', 'test'))
        for index in range(4):
            conflict = list(profiles)
            conflict[index] = {'log': {fact, ''}}
            self.assertNotIn('{"log",', render(conflict, exports, 'test', 'test'))


if __name__ == '__main__':
    unittest.main()
