import json
import unittest
from types import SimpleNamespace

from scripts.generate_darwin_record_data_declarations import (
    RecordDeclarations, common_record, render)


class DarwinRecordDataDeclarationsTests(unittest.TestCase):
    @staticmethod
    def fact(size=16, alignment=8, encoding="{CGSize=dd}"):
        return json.dumps((encoding, size, alignment))

    def test_exact_record_profiles_and_exports_preserve_type_size_and_alignment(self):
        profile = {"CGSizeZero": {self.fact()}}
        output, count = render([profile] * 4, [{"CGSizeZero": {"B", "A"}}] * 2,
                               "test", "test")
        self.assertEqual(count, 1)
        self.assertIn('{"CGSizeZero", "{CGSize=dd}", 16, 8, "A|B", '
                      '"{CGSize=dd}", 16, 8, "A|B"}', output)
        self.assertIn("No object contents, initialization or call effects", output)

    def test_missing_conflicting_and_alternative_facts_do_not_grant_a_layout(self):
        profile = {"CGSizeZero": {self.fact()}}
        for negative in ({}, {"CGSizeZero": {""}},
                         {"CGSizeZero": {self.fact(24)}},
                         {"CGSizeZero": {self.fact(16, 16)}},
                         {"CGSizeZero": {self.fact(), self.fact(32)}}):
            with self.subTest(negative=negative):
                output, _ = render([profile, negative, profile, negative],
                                   [{"CGSizeZero": {"A"}}] * 2, "test", "test")
                self.assertNotIn('{"CGSizeZero", "{CGSize=dd}"', output)
        _, count = render([profile] * 4, [{}] * 2, "test", "test")
        self.assertEqual(count, 0)
        alternative = {"CGSizeZero": {self.fact(), self.fact(32)}}
        _, count = render([alternative] * 4, [{"CGSizeZero": {"A"}}] * 2,
                          "test", "test")
        self.assertEqual(count, 0)

    def test_exports_and_platform_agreement_remain_architecture_specific(self):
        profile = {"CGSizeZero": {self.fact()}}
        output, count = render([profile, profile, profile, {}],
                               [{"CGSizeZero": {"A"}}, {"CGSizeZero": {"B"}}],
                               "test", "test")
        self.assertEqual(count, 1)
        self.assertIn('"{CGSize=dd}", 16, 8, "A", nullptr, 0, 0, ""', output)

    def test_invalid_extent_alignment_and_encoding_are_negative_evidence(self):
        for value in ("", "invalid", "null", "[]", self.fact(0),
                      self.fact(65536), self.fact(16, 3), self.fact(16, 32),
                      self.fact(True), self.fact(16, True),
                      self.fact(16, 8, "^v")):
            with self.subTest(value=value):
                self.assertIsNone(common_record(value))

    def test_only_external_non_tls_complete_record_declarations_supply_facts(self):
        clang = RecordDeclarations.__new__(RecordDeclarations)
        clang.string = lambda value: value
        clang.clang_getCursorLinkage = lambda cursor: cursor.linkage
        clang.clang_Cursor_getMangling = lambda cursor: cursor.name
        clang.clang_getCursorTLSKind = lambda cursor: cursor.tls
        clang.clang_getCursorType = lambda cursor: cursor.type
        clang.clang_getCanonicalType = lambda value: value
        clang.clang_getDeclObjCTypeEncoding = lambda cursor: cursor.encoding
        clang.clang_Type_getSizeOf = lambda value: value.size
        clang.clang_Type_getAlignOf = lambda value: value.alignment
        cursor = SimpleNamespace(kind=9, linkage=4, name="_CGSizeZero", tls=0,
                                 encoding="{CGSize=dd}",
                                 type=SimpleNamespace(kind=105, size=16, alignment=8))
        self.assertEqual(clang.declaration(cursor), ("CGSizeZero", self.fact()))
        for field, value in (("tls", 1), ("tls", 2), ("encoding", "^v")):
            changed = SimpleNamespace(**vars(cursor))
            setattr(changed, field, value)
            self.assertEqual(clang.declaration(changed), ("CGSizeZero", ""))
        for field, value in (("kind", 8), ("linkage", 2), ("name", "_unrelated")):
            changed = SimpleNamespace(**vars(cursor))
            setattr(changed, field, value)
            self.assertIsNone(clang.declaration(changed))
        for field, value in (("kind", 17), ("size", -1), ("alignment", -1)):
            changed = SimpleNamespace(**vars(cursor))
            changed.type = SimpleNamespace(**vars(cursor.type))
            setattr(changed.type, field, value)
            self.assertEqual(clang.declaration(changed), ("CGSizeZero", ""))


if __name__ == "__main__":
    unittest.main()
