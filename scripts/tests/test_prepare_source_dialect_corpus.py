import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts.prepare_source_dialect_corpus import prepare


class SourceDialectCorpusTests(unittest.TestCase):
    def test_missing_cli_fails_before_creating_a_corpus(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(FileNotFoundError):
                prepare(root / "missing-neverd", root / "output")
            self.assertFalse((root / "output").exists())

    def test_success_without_source_cannot_reuse_a_stale_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            neverd = root / "neverd"
            neverd.touch()
            corpus = root / "output" / "corpus"
            corpus.mkdir(parents=True)
            stale = corpus / "x64-c.c"
            stale.write_text("int stale(void) { return 7; }", encoding="utf-8")
            result = subprocess.CompletedProcess([], 0, "", "")
            with patch("scripts.prepare_source_dialect_corpus.subprocess.run",
                       return_value=result):
                with self.assertRaisesRegex(RuntimeError, "no C source"):
                    prepare(neverd, root / "output")
            self.assertFalse(stale.exists())

    def test_decompile_failure_keeps_its_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            neverd = root / "neverd"
            neverd.touch()
            result = subprocess.CompletedProcess([], 9, "loading fixture\n",
                                                  "unsupported fixture\n")
            with patch("scripts.prepare_source_dialect_corpus.subprocess.run",
                       return_value=result):
                with self.assertRaisesRegex(RuntimeError, "unsupported fixture"):
                    prepare(neverd, root / "output")
            self.assertEqual(
                (root / "output" / "x64-c.log").read_text(encoding="utf-8"),
                result.stdout + result.stderr,
            )


if __name__ == "__main__":
    unittest.main()
