"""Source-collision refinements preserve uncertainty and library selection."""

from pathlib import Path
import tempfile
import unittest

from scripts.signatures import build_msvc_signatures as builder
from scripts.signatures import refine_colliding_patterns as refinement


class CollisionRefinementTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.existing = self.root / "existing"
        self.existing.mkdir()
        self.output = self.root / "output"
        self.evidence = self.root / "source.pat"

    def run_refinement(self, files, evidence, machine="x64"):
        for name, text in files.items():
            (self.existing / name).write_text(text + "\n---\n")
        self.evidence.write_text(evidence + "\n---\n")
        return refinement.refine(self.existing, [self.evidence], self.output, machine)

    def test_indistinguishable_names_become_incoming_queries_not_aliases(self):
        line = "488B05........C3 00 0000 0008 :0000 first &0003 heap"
        source = line.replace(":0000 first", "!evidence :0000 second")
        result = self.run_refinement({"runtime.pat": line}, source)
        self.assertEqual(result["counts"], {"caller": 1})
        rows = [builder.parse_line(text) for text in (self.output / "runtime.npat").read_text().splitlines()]
        rows = [row for row in rows if row]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].names, (":0000 first",))
        self.assertTrue(rows[0].require_caller)
        self.assertNotIn(":0000 first", (self.output / "runtime.pat").read_text())

    def test_different_reference_targets_need_confirmation(self):
        line = "E8........C3 00 0000 0006 :0000 first ^0001 original"
        source = line.replace(":0000 first", "!evidence :0000 second").replace("original", "different")
        result = self.run_refinement({"runtime.pat": line}, source)
        self.assertEqual(result["counts"], {"required": 1})
        self.assertIn("!refs :0000 first ^0001 original", (self.output / "runtime.npat").read_text())

    def test_known_same_library_cohorts_survive_across_parts(self):
        body = "E8........C3 00 0000 0006"
        first = body + " :0000 first ^0001 original"
        second = body + " :0000 second ^0001 different"
        source = second.replace(" :0000", " !evidence :0000")
        result = self.run_refinement({"runtime.pat": first, "runtime.part2.pat": second}, source)
        self.assertEqual(result["counts"], {})
        self.assertEqual((self.output / "runtime.pat").read_text(), first + "\n---\n")

    def test_an_independently_selected_library_cannot_hide_a_counterclaim(self):
        body = "E8........C3 00 0000 0006"
        first = body + " :0000 first ^0001 original"
        second = body + " :0000 second ^0001 different"
        source = second.replace(" :0000", " !evidence :0000")
        result = self.run_refinement({"vs2017.pat": first, "vs2022.pat": second}, source)
        self.assertEqual(result["counts"], {"required": 1})
        self.assertEqual(result["actions"][0]["file"], "vs2017.pat")

    def test_known_name_with_different_source_references_is_still_a_counterclaim(self):
        body = "E8........C3 00 0000 0006"
        first = body + " :0000 first ^0001 original"
        second = body + " :0000 second ^0001 different"
        source = (body + " !evidence :0000 second ^0001 original")
        result = self.run_refinement({"runtime.pat": first + "\n" + second}, source)
        self.assertEqual(result["counts"], {"caller": 1})

    def test_existing_required_claim_cannot_ignore_new_indistinguishable_source(self):
        line = "E8........C3 00 0000 0006 !refs :0000 first ^0001 original"
        source = "E8........C3 00 0000 0006 !evidence :0000 second ^0001 original"
        result = self.run_refinement({"runtime.npat": line}, source)
        self.assertEqual(result["counts"], {"omitted": 1})
        self.assertNotIn(":0000 first", (self.output / "runtime.npat").read_text())

    def test_existing_required_claim_survives_distinct_source_references(self):
        line = "E8........C3 00 0000 0006 !refs :0000 first ^0001 original"
        source = "E8........C3 00 0000 0006 !evidence :0000 second ^0001 different"
        result = self.run_refinement({"runtime.npat": line}, source)
        self.assertEqual(result["counts"], {})
        self.assertEqual((self.output / "runtime.npat").read_text(), line + "\n---\n")

    def test_thumb_padding_query_includes_shorter_literal_source_prefixes(self):
        body = "2DE9F84F0DF1200B93E8F0079047BDE8F88F"
        line = body + "0000 00 0000 0014 :0000 filter"
        source = body + " 00 0000 0012 !evidence :0000 termination"
        result = self.run_refinement({"runtime.pat": line}, source, "arm")
        self.assertEqual(result["counts"], {"caller": 1})

    def test_source_can_state_the_same_bytes_with_a_shorter_leading_field(self):
        line = "AABBCCDDEEFF0011 00 0000 0008 :0000 first"
        source = "AABBCCDD 00 0000 0008 !evidence :0000 second EEFF0011"
        result = self.run_refinement({"runtime.pat": line}, source)
        self.assertEqual(result["counts"], {"caller": 1})

    def test_wildcards_cover_crc_bytes_but_do_not_claim_their_values(self):
        lead = "90" * 32
        line = f"{lead} 00 0000 002C :0000 loose " + ".." * 8 + "AABBCCDD"
        source = f"{lead} 08 1234 002C !evidence :0000 precise AABBCCDD"
        result = self.run_refinement({"runtime.pat": line}, source)
        self.assertEqual(result["counts"], {"caller": 1})

    def test_partial_conflicting_records_cannot_be_promoted_to_context(self):
        line = "AABBCCDD 00 0000 0008 :0000 partial"
        source = "AABBCCDD112233C3 00 0000 0008 !evidence :0000 complete"
        result = self.run_refinement({"runtime.pat": line}, source)
        self.assertEqual(result["counts"], {"omitted": 1})
        self.assertFalse((self.output / "runtime.npat").exists())

    def test_real_alias_and_explicit_constraints_remain_unchanged(self):
        line = "E8........C3 00 0000 0006 :0000 first :0000 shared ^0001 original"
        constrained = line.replace(" :0000 first", " !refs :0000 first")
        source = line.replace(":0000 first", "!evidence :0000 second")
        result = self.run_refinement({"runtime.pat": line, "runtime.part2.npat": constrained}, source)
        self.assertEqual(result["counts"], {})

    def test_pruned_sources_and_existing_destinations_fail_before_writes(self):
        line = "AABBCCDD 00 0000 0004 :0000 first"
        with self.assertRaisesRegex(builder.BuildError, "unpruned"):
            self.run_refinement({"runtime.pat": line}, line)
        self.assertFalse(self.output.exists())
        self.output.mkdir()
        sentinel = self.output / "keep"
        sentinel.write_text("unchanged")
        with self.assertRaisesRegex(builder.BuildError, "already exists"):
            refinement.refine(self.existing, [self.evidence], self.output, "x64")
        self.assertEqual(sentinel.read_text(), "unchanged")


if __name__ == "__main__":
    unittest.main()
