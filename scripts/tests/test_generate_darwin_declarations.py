import unittest
from scripts.generate_darwin_declarations import export_index, render


class DarwinDeclarationTests(unittest.TestCase):
    def document(self, module, symbols=(), dependencies=(), target="arm64-macos"):
        return {"tbd-version": 4, "targets": [target], "install-name": module,
                "exports": [{"targets": [target], "symbols": list(symbols)}],
                "reexported-libraries": [{"targets": [target], "libraries": list(dependencies)}]}

    def test_reexports_reach_fixed_point_without_cross_target_leakage(self):
        docs = [self.document("A", ["_a"], ["B"]),
                self.document("B", ["_b"], ["A", "C"]),
                self.document("C", ["_c"]),
                self.document("D", ["_d"], target="x86_64-macos")]
        expected = {"a": {"A", "B"}, "b": {"A", "B"}, "c": {"A", "B", "C"}}
        self.assertEqual(export_index(docs, "arm64-macos"), expected)
        self.assertEqual(export_index(list(reversed(docs)), "arm64-macos"), expected)
        self.assertEqual(export_index(docs, "x86_64-macos"), {"d": {"D"}})

    def test_unknown_tbd_version_fails_instead_of_losing_export_rules(self):
        doc = self.document("A")
        doc["tbd-version"] = 5
        with self.assertRaises(ValueError):
            export_index([doc], "arm64-macos")

    def test_declaration_does_not_authorize_an_unexported_symbol(self):
        profile = {"known": {"i0"}, "unknown": {"i0"}}
        output, count = render([profile] * 4, [{"known": {"A"}}] * 2, "test", "test")
        self.assertEqual(count, 1)
        self.assertIn('{"known", "i0", "i0", "A", "A"}', output)
        self.assertNotIn('"unknown"', output)

    def test_conflict_variadic_and_absent_profiles_never_gain_positive_facts(self):
        first = {"conflict": {"i0"}, "variadic": {""}, "absent": {"i0"}}
        second = {"conflict": {"v0"}, "variadic": {""}}
        exports = [{name: {"A"} for name in first}] * 2
        output, count = render([first, second, first, second], exports, "test", "test")
        self.assertEqual(count, 2)
        self.assertIn('{"conflict", "", "", "A", "A"}', output)
        self.assertIn('{"variadic", "", "", "A", "A"}', output)
        self.assertNotIn('"absent"', output)


if __name__ == "__main__":
    unittest.main()
