"""Independent truth, result-admission and tool-format regression checks."""
import copy
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest

from scripts import benchmark_binary_diff as bench


class MappingTests(unittest.TestCase):
    def test_wrong_and_missed_are_not_disjoint(self):
        result = bench.score({(1, 3), (2, 4)}, [(1, 4), (2, 3)])
        self.assertEqual((result["correct"], result["wrong"], result["missed"]), (0, 2, 2))
        self.assertEqual(result["precision"], 0)
        self.assertEqual(result["recall"], 0)
        self.assertFalse(result["exact_mapping"])

    def test_repeated_correlator_evidence_does_not_inflate_score(self):
        result = bench.score({(1, 3), (2, 4)}, [(1, 3), (1, 3), (2, 4)])
        self.assertEqual(result["accepted_pairs"], 2)
        self.assertEqual(result["duplicate_evidence_rows"], 1)
        self.assertEqual(result["correct"], 2)
        self.assertTrue(result["exact_mapping"])

    def test_conflicting_associations_are_not_overwritten(self):
        result = bench.score({(1, 3), (2, 4)}, [(1, 3), (1, 4)])
        self.assertEqual((result["correct"], result["wrong"], result["missed"]), (1, 1, 1))
        self.assertEqual(result["wrong_pairs"], [["0x1", "0x4"]])

    def test_abstention_has_undefined_precision(self):
        result = bench.score({(1, 3)}, [])
        self.assertIsNone(result["precision"])
        self.assertEqual(result["recall"], 0)
        self.assertFalse(result["exact_mapping"])

    def test_unrelated_functions_have_no_expected_pair(self):
        expected = bench.truth_pairs([
            {"id": "removed", "primary": "0x1000", "secondary": None},
            {"id": "added", "primary": None, "secondary": "0x2000"},
        ])
        result = bench.score(expected, [(0x1000, 0x2000)])
        self.assertEqual(result["wrong"], 1)
        self.assertIsNone(result["recall"])
        self.assertFalse(result["exact_mapping"])

    def test_invalid_truth_is_not_silently_reduced(self):
        row = {"id": "one", "primary": "0x1000", "secondary": "0x2000"}
        cases = [[], [row, row], [row, dict(row, id="other")],
                 [dict(row, primary=None, secondary=None)], [dict(row, id="")]]
        for rows in cases:
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                bench.truth_pairs(rows)

    def test_address_formats_and_bounds(self):
        self.assertEqual(bench.address("0x1000"), 4096)
        self.assertEqual(bench.address("001000", 16), 4096)
        self.assertEqual(bench.address("001000", 10), 1000)
        self.assertEqual(bench.address((1 << 64) - 1), (1 << 64) - 1)
        for value in (True, -1, 1 << 64, 1.5, None):
            with self.subTest(value=value), self.assertRaises(ValueError):
                bench.address(value)


class ArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.inputs = [self.root / "primary.bin", self.root / "secondary.bin"]
        self.inputs[0].write_bytes(b"original input")
        self.inputs[1].write_bytes(b"changed input")
        self.report = {
            "schema": "neverd.bindiff", "schema_version": 3, "state": "complete",
            "primary": {"sha256": bench.digest(self.inputs[0])},
            "secondary": {"sha256": bench.digest(self.inputs[1])},
            "matches": [{"primary": "0x1000", "secondary": "0x2000"}],
            "ambiguities": [{"primary": "0x1000", "secondary": ["0x3000"]}],
        }
        self.case = {
            "id": "owned", "category": "test", "truth_complete": True,
            "primary": bench.artifact(self.inputs[0], self.root),
            "secondary": bench.artifact(self.inputs[1], self.root),
            "truth": [{"id": "entry", "primary": "0x1000", "secondary": "0x2000"}],
        }

    def write(self, name, data):
        path = self.root / name
        path.write_text(json.dumps(data))
        return path

    def receipt(self):
        path = self.write("report.json", self.report)
        run = {
            "schema": bench.RUN_SCHEMA, "case": "owned", "format": "neverd",
            "track": "end-to-end", "completed": True, "tool": {"name": "NeverD", "version": "test"},
            "input_sha256": [bench.digest(p) for p in self.inputs],
            "result": bench.artifact(path, self.root), "artifacts": [],
        }
        corpus_path = self.write("corpus.json", {"schema": bench.CORPUS_SCHEMA, "cases": [self.case]})
        return corpus_path, self.write("run.json", run), run

    def test_complete_report_scores_only_accepted_matches(self):
        corpus, run, _ = self.receipt()
        result = bench.evaluate(corpus, run)
        self.assertTrue(result["metrics"]["exact_mapping"])
        self.assertEqual(result["metrics"]["accepted_pairs"], 1)
        self.assertEqual(result["input_binding"], "native-and-receipt")

    def test_changed_input_or_result_is_rejected(self):
        for target in ("primary.bin", "report.json"):
            with self.subTest(target=target):
                corpus, run, _ = self.receipt()
                original = (self.root / target).read_bytes()
                (self.root / target).write_bytes(original + b" ")
                with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                    bench.evaluate(corpus, run)
                (self.root / target).write_bytes(original)

    def test_swapped_native_binding_is_rejected(self):
        self.report["primary"], self.report["secondary"] = self.report["secondary"], self.report["primary"]
        corpus, run, _ = self.receipt()
        with self.assertRaisesRegex(ValueError, "input identity"):
            bench.evaluate(corpus, run)

    def test_partial_native_result_is_rejected_even_with_success_receipt(self):
        for state in ("cancelled", "resource_limited", "deadline"):
            with self.subTest(state=state):
                self.report["state"] = state
                corpus, run, _ = self.receipt()
                with self.assertRaisesRegex(ValueError, "did not complete"):
                    bench.evaluate(corpus, run)

    def test_run_admission(self):
        for field, value, message in (("completed", False, "did not complete"),
                                       ("input_sha256", [], "direction"),
                                       ("track", "invented", "track"),
                                       ("tool", {"name": "NeverD", "version": ""}, "version")):
            with self.subTest(field=field):
                corpus, run_path, run = self.receipt()
                run[field] = value
                self.write("run.json", run)
                with self.assertRaisesRegex(ValueError, message):
                    bench.evaluate(corpus, run_path)

    def test_partial_ground_truth_is_not_treated_as_complete(self):
        self.case["truth_complete"] = False
        corpus, run, _ = self.receipt()
        with self.assertRaisesRegex(ValueError, "complete function truth"):
            bench.evaluate(corpus, run)

    def test_stale_runtime_artifact_is_rejected(self):
        corpus, run_path, run = self.receipt()
        tool = self.root / "tool"
        tool.write_bytes(b"version one")
        run["artifacts"] = [bench.artifact(tool, self.root)]
        self.write("run.json", run)
        tool.write_bytes(b"version two")
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            bench.evaluate(corpus, run_path)

    def test_ghidra_accepts_functions_only_and_retains_correlator_duplicates(self):
        row = {"source": "4096", "destination": "8192", "source_space": "ram",
               "destination_space": "ram", "type": "Function", "status": "ACCEPTED"}
        report = {"schema": "neverd.baseline.ghidra-vt-smoke.v1", "match_sets": [
            {"matches": [row, dict(row, type="Data"), dict(row, status="AVAILABLE")]},
            {"matches": [copy.deepcopy(row)]},
        ]}
        path = self.write("ghidra.json", report)
        self.assertEqual(bench.read_pairs("ghidra", path, self.inputs), [(4096, 8192), (4096, 8192)])
        row["source_space"] = "overlay"
        self.write("ghidra.json", report)
        with self.assertRaisesRegex(ValueError, "address-space"):
            bench.read_pairs("ghidra", path, self.inputs)

    def test_diaphora_uses_hex_and_excludes_unreliable_and_multimatch(self):
        path = self.root / "result.diaphora"
        with sqlite3.connect(path) as db:
            db.execute("CREATE TABLE results (type TEXT, address TEXT, address2 TEXT)")
            db.executemany("INSERT INTO results VALUES (?, ?, ?)", [
                ("best", "001000", "002000"), ("partial", "001010", "002010"),
                ("unreliable", "001100", "002100"), ("multimatch", "001200", "002200"),
            ])
        self.assertEqual(bench.read_pairs("diaphora", path, self.inputs), [(4096, 8192), (4112, 8208)])
        with sqlite3.connect(path) as db:
            db.execute("INSERT INTO results VALUES ('new category', '0', '0')")
        with self.assertRaisesRegex(ValueError, "unknown Diaphora"):
            bench.read_pairs("diaphora", path, self.inputs)

    def test_native_ghidra_export_requires_the_actual_input_pair(self):
        report = {
            "schema": "neverd.benchmark.ghidra-vt.v1", "complete": True,
            "source_sha256": bench.digest(self.inputs[0]),
            "destination_sha256": bench.digest(self.inputs[1]),
            "match_sets": [{"matches": [{"source": "4096", "destination": "8192",
                "source_space": "ram", "destination_space": "ram", "type": "Function", "status": "ACCEPTED"}]}],
        }
        path = self.write("native-ghidra.json", report)
        self.assertEqual(bench.read_pairs("ghidra-vt", path, self.inputs), [(4096, 8192)])
        with self.assertRaisesRegex(ValueError, "input identity"):
            bench.read_pairs("ghidra-vt", path, list(reversed(self.inputs)))
        report["complete"] = False
        self.write("native-ghidra.json", report)
        with self.assertRaisesRegex(ValueError, "did not complete"):
            bench.read_pairs("ghidra-vt", path, self.inputs)

    def test_bindiff_uses_metadata_orientation_and_unsigned_addresses(self):
        path = self.root / "result.BinDiff"
        with sqlite3.connect(path) as db:
            db.execute("CREATE TABLE metadata (file1 INT, file2 INT)")
            db.execute("CREATE TABLE file (id INT, hash TEXT)")
            db.execute("CREATE TABLE function (address1 BIGINT, address2 BIGINT)")
            db.execute("INSERT INTO metadata VALUES (8, 7)")
            db.executemany("INSERT INTO file VALUES (?, ?)", [
                (7, bench.digest(self.inputs[1], "sha1")), (8, bench.digest(self.inputs[0]))])
            db.execute("INSERT INTO function VALUES (-1, 8192)")
        self.assertEqual(bench.read_pairs("bindiff", path, self.inputs), [((1 << 64) - 1, 8192)])
        with self.assertRaisesRegex(ValueError, "input identity"):
            bench.read_pairs("bindiff", path, list(reversed(self.inputs)))

    def test_map_keeps_same_named_functions_bound_to_their_object(self):
        path = self.root / "link.map"
        path.write_text(""" VMA LMA Size Align Out In Symbol
 1000 1000 20 16 /fixture with spaces/one.o:(.text)
 1000 1000 10 1 one
 1010 1010 10 1 helper
 1020 1020 20 16 /fixture/two.o:(.text)
 1020 1020 10 1 two
 1030 1030 10 1 helper
""")
        expected = {"one.o": {"one", "helper"}, "two.o": {"two", "helper"}}
        self.assertEqual(bench.linker_truth(path, expected), {
            "one.o:one": 0x1000, "one.o:helper": 0x1010,
            "two.o:two": 0x1020, "two.o:helper": 0x1030,
        })
        with self.assertRaisesRegex(ValueError, "missing authored"):
            bench.linker_truth(path, {"one.o": {"missing"}})

    def test_save_never_overwrites_prior_evidence(self):
        path = self.root / "result.json"
        bench.save(path, {"original": True})
        with self.assertRaises(FileExistsError):
            bench.save(path, {"original": False})
        self.assertEqual(bench.load(path), {"original": True})


if __name__ == "__main__":
    unittest.main()
