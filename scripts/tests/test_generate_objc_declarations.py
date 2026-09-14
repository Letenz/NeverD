import unittest

from scripts.generate_objc_declarations import catalog_rows, common_encodings


class ObjCDeclarationCatalogTests(unittest.TestCase):
    def test_platform_disagreement_differs_from_missing_declarations(self):
        arm = {"predicate:": {"B24@0:8@16"}}
        intel = {"predicate:": {"c24@0:8@16"}}
        self.assertEqual(common_encodings(arm, intel, "predicate:"), [""])
        self.assertEqual(common_encodings(arm, {}, "predicate:"), [None])
        self.assertEqual(list(catalog_rows([arm, {}, arm, {}])), [])

    def test_variadic_declaration_vetoes_fixed_prefix_in_any_order(self):
        for encodings in ({"", "@24@0:8@16"}, {"@24@0:8@16", ""}):
            declarations = {"format:": encodings}
            self.assertEqual(common_encodings(declarations, declarations,
                                               "format:"), [""])

    def test_all_signature_alternatives_survive_deterministic_generation(self):
        arm = {"value": {"Q16@0:8", "q16@0:8"}, "item": {"@16@0:8"}}
        intel = {"value": {"q16@0:8"}, "item": {"@16@0:8"}}
        rows = list(catalog_rows([arm, arm, intel, intel]))
        self.assertEqual(rows, [
            ("item", "@16@0:8", "@16@0:8"),
            ("value", "Q16@0:8", "q16@0:8"),
            ("value", "q16@0:8", "q16@0:8"),
        ])
        reverse = dict(reversed(list(arm.items())))
        self.assertEqual(rows, list(catalog_rows([reverse, arm, intel, intel])))

    def test_missing_target_profile_cannot_generate_a_catalog(self):
        with self.assertRaises(ValueError):
            list(catalog_rows([{"item": {"@16@0:8"}}]))


if __name__ == "__main__":
    unittest.main()
