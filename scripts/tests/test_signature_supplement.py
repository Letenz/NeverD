"""Supplement selection keeps raw counterexamples and existing ownership."""

from pathlib import Path
import tempfile
import unittest

from scripts.signatures import build_msvc_signatures as builder
from scripts.signatures import supplement_patterns as supplement


class SupplementTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.existing = self.root / "existing"
        self.existing.mkdir()

    def select(self, candidates, evidence, existing=""):
        (self.existing / "old.pat").write_text(existing)
        paths = []
        for index, (name, text) in enumerate(evidence):
            path = self.root / f"raw{index}.pat"
            path.write_text(text)
            paths.append((name, path))
        return supplement.select({"runtime": builder.fold(candidates.splitlines())},
                                 paths, self.existing)["runtime"]

    def test_discarded_source_collision_is_still_evidence(self):
        claim = "AABBCCDD 00 0000 0004 :0000 wanted\n"
        conflict = "AABBCCDD 00 0000 0004 :0000 other\n"
        self.assertEqual(self.select(claim, [("runtime", claim + conflict)]), [])

    def test_covering_source_routine_cannot_be_reintroduced(self):
        claim = "AABBCCDD.... 00 0000 0006 :0000 wanted\n"
        covering = "AABBCCDDEEFF11 00 0000 0007 :0000 other\n"
        self.assertEqual(self.select(claim, [("runtime", claim), ("another", covering)]), [])

    def test_references_can_distinguish_same_library_claims(self):
        a = "AABBCCDD.... 00 0000 0006 :0000 a ^0004 __imp_a\n"
        b = "AABBCCDD.... 00 0000 0006 :0000 b ^0004 __imp_b\n"
        lines = self.select(a + b, [("runtime", a), ("runtime", b)])
        self.assertEqual({line.names for line in lines}, {(':0000 a',), (':0000 b',)})
        self.assertEqual(self.select(a, [("runtime", a), ("another", b)]), [])

    def test_aliases_do_not_duplicate_an_existing_claim(self):
        aliases = "AABBCCDD 00 0000 0004 :0000 copy :0000 move\n"
        old = "AABBCCDD 00 0000 0004 :0000 copy\n"
        self.assertEqual(self.select(aliases, [("runtime", aliases)], old), [])
        self.assertEqual((self.existing / "old.pat").read_text(), old)

    def test_relocated_leading_bytes_still_check_exact_collisions(self):
        claim = "....AABB 00 0000 0004 :0000 wanted\n"
        conflict = "....AABB 00 0000 0004 :0000 other\n"
        self.assertEqual(self.select(claim, [("runtime", claim + conflict)]), [])

    def test_candidate_needs_its_own_raw_source(self):
        claim = "AABBCCDD 00 0000 0004 :0000 wanted\n"
        with self.assertRaisesRegex(builder.BuildError, "lacks raw source evidence"):
            self.select(claim, [("another", claim)])

    def test_numbered_output_does_not_replace_existing_parts(self):
        claim = "AABBCCDD 00 0000 0004 :0000 wanted\n"
        source = self.root / "raw.pat"
        source.write_text(claim)
        old = self.existing / "runtime.part2.pat"
        old.write_text("11223344 00 0000 0004 :0000 old\n")
        self.assertEqual(supplement.main([
            "--candidate", f"runtime={source}", "--evidence", f"runtime={source}",
            "--existing", str(self.existing), "--output", str(self.existing)]), 0)
        self.assertIn(":0000 old", old.read_text())
        self.assertEqual((self.existing / "runtime.part3.pat").read_text(), claim + "---\n")


if __name__ == "__main__":
    unittest.main()
