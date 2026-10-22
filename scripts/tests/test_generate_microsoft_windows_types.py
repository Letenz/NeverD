"""Official header generation rejects partial or ABI-changing inputs."""

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
SPEC = importlib.util.spec_from_file_location(
    "microsoft_windows_types", SCRIPTS / "generate_microsoft_windows_types.py")
windows = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(windows)


class MicrosoftWindowsTypes(unittest.TestCase):
    def test_layout_changes_reject_equal_size_and_pointer_width_loss(self):
        original = ("*** Dumping AST Record Layout\n"
                    "         0 | struct Record\n"
                    "         0 |   void * __ptr32 Pointer\n"
                    "         4 |   unsigned int Length\n"
                    "           | [sizeof=8, align=4]\n")
        self.assertEqual(windows.verify_layouts(original, original), 1)
        for changed in (original.replace("align=4", "align=8"),
                        original.replace("4 |", "8 |"),
                        original.replace("sizeof=8", "sizeof=16")):
            with self.assertRaisesRegex(ValueError, "Record"):
                windows.verify_layouts(original, changed)
        with self.assertRaisesRegex(ValueError, "no shared"):
            windows.verify_layouts(original, "")

    def test_failed_update_cannot_certify_stale_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "manifest.json").write_text('{"stale": true}')
            with patch.object(windows.wdk, "declarations", side_effect=ValueError("fixture")):
                with self.assertRaisesRegex(ValueError, "fixture"):
                    windows.generate(root, root / "cache", "neverd", "clang")
            self.assertFalse((root / "manifest.json").exists())

    def test_partial_parser_results_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "parse.log"
            with patch.object(windows, "run", return_value='{"problems": [{"message": "unknown type"}]}'):
                with self.assertRaisesRegex(ValueError, "refused"):
                    windows.checked_types("neverd", Path("test.h"), log)
            self.assertIn("unknown type", log.read_text())


if __name__ == "__main__":
    unittest.main()
