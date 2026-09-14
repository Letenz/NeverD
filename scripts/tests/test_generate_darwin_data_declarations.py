import unittest
from types import SimpleNamespace
from scripts.generate_darwin_data_declarations import DataDeclarations, render


class DarwinDataDeclarationTests(unittest.TestCase):
    def test_only_external_non_tls_variables_supply_storage_addresses(self):
        clang = DataDeclarations.__new__(DataDeclarations)
        clang.string = lambda value: value
        clang.clang_getCursorLinkage = lambda cursor: cursor.linkage
        clang.clang_Cursor_getMangling = lambda cursor: cursor.name
        clang.clang_getCursorTLSKind = lambda cursor: cursor.tls
        cursor = SimpleNamespace(kind=9, linkage=4, name="_value", tls=0)
        self.assertEqual(clang.declaration(cursor), ("value", "data"))
        for tls in (1, 2):
            cursor.tls = tls
            self.assertEqual(clang.declaration(cursor), ("value", ""))
        for field, value in (("kind", 8), ("linkage", 2), ("name", "value")):
            changed = SimpleNamespace(**vars(cursor))
            setattr(changed, field, value)
            self.assertIsNone(clang.declaration(changed))

    def test_missing_conflicting_and_tls_profiles_cannot_bind(self):
        first = {"tls": {""}, "mixed": {"data", ""}, "absent": {"data"},
                 "conflict": {"data"}, "known": {"data"}}
        second = {"tls": {""}, "mixed": {"data", ""}, "conflict": {""},
                  "known": {"data"}}
        output, count = render([first, second, first, second],
                               [{name: {"A"} for name in first}] * 2, "test", "test")
        self.assertEqual(count, 4)
        self.assertNotIn('"absent"', output)
        for name in ("tls", "mixed", "conflict"):
            self.assertIn('{"' + name + '", "", ""}', output)
        self.assertIn('{"known", "A", "A"}', output)

    def test_exports_remain_architecture_specific_and_deterministic(self):
        profile = {"first": {"data"}, "second": {"data"}, "missing": {"data"}}
        exports = [{"first": {"B", "A"}}, {"second": {"C"}}]
        output, count = render([profile] * 4, exports, "test", "test")
        self.assertEqual(count, 2)
        self.assertIn('{"first", "A|B", ""}', output)
        self.assertIn('{"second", "", "C"}', output)
        self.assertNotIn('"missing"', output)
        reverse = dict(reversed(list(profile.items())))
        self.assertEqual(render([reverse] * 4, exports, "test", "test")[0], output)


if __name__ == "__main__":
    unittest.main()
