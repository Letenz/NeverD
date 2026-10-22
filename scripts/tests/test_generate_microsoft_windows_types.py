"""Official header generation rejects partial or ABI-changing inputs."""

import importlib.util
import json
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
    def test_x86_driver_compiler_default_requires_official_build_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            kit = Path(directory)
            props = kit / f"c/build/{windows.VERSION}/WindowsDriver.Common.props"
            props.parent.mkdir(parents=True)
            source = ('<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">'
                      '<ItemDefinitionGroup><ClCompile>'
                      '<CallingConvention>StdCall</CallingConvention>'
                      '</ClCompile></ItemDefinitionGroup></Project>')
            props.write_text(source)
            self.assertEqual(windows.compiler_defaults(kit, "x86", "WindowsKernel"),
                             (["-Xclang", "-fdefault-calling-conv=stdcall"], "__stdcall"))
            for arch in windows.TARGETS:
                self.assertEqual(windows.compiler_defaults(kit, arch, "WindowsUser"), ([], ""))
                if arch != "x86":
                    self.assertEqual(windows.compiler_defaults(kit, arch, "WindowsKernel"), ([], ""))
            props.write_text(source.replace("StdCall", "unknown"))
            with self.assertRaisesRegex(ValueError, "unrecognized WDK"):
                windows.compiler_defaults(kit, "x86", "WindowsKernel")

    def test_combined_targets_require_all_headers_and_matching_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            targets = []
            for arch, (suffix, _, _, _) in windows.TARGETS.items():
                target = root / arch
                (target / "licenses").mkdir(parents=True)
                notice = target / "licenses" / "notice.txt"
                notice.write_text("Synthetic test notice")
                libraries = []
                for base in windows.LIBRARIES:
                    name = base + suffix
                    header = target / (name + ".h")
                    header.write_text("/* synthetic " + arch + " library */")
                    libraries.append({"name": name, "sha256": windows.wdk.digest(header)})
                record = {"schema": 1, "provider": "Microsoft SDK/WDK",
                          "architecture": arch, "libraries": libraries,
                          "compiler": "fixture compiler", "packages": [],
                          "licenses": [{"file": notice.name, "sha256": windows.wdk.digest(notice)}]}
                (target / "manifest.json").write_text(json.dumps(record))
                targets.append(target)
            output = root / "combined"
            windows.combine_targets(output, targets)
            combined = json.loads((output / "manifest.json").read_text())
            self.assertEqual(len(combined["libraries"]), 6)
            self.assertEqual({entry["architecture"] for entry in combined["libraries"]},
                             set(windows.TARGETS))
            for record in combined["libraries"]:
                self.assertEqual(windows.wdk.digest(output / (record["name"] + ".h")), record["sha256"])
            with self.assertRaisesRegex(ValueError, "required"):
                windows.combine_targets(output, targets[:2])
            self.assertFalse((output / "manifest.json").exists())
            header = targets[0] / "WindowsKernelX64.h"
            header.write_text("changed bytes")
            with self.assertRaisesRegex(ValueError, "changed Microsoft target header"):
                windows.combine_targets(output, targets)
            self.assertFalse((output / "manifest.json").exists())

    def test_layout_changes_reject_equal_size_and_pointer_width_loss(self):
        original = ("*** Dumping AST Record Layout\n"
                    "         0 | struct Record\n"
                    "         0 |   void * __ptr32 Pointer\n"
                    "         4 |   unsigned int Length\n"
                    "           | [sizeof=8, align=4]\n")
        self.assertEqual(windows.verify_layouts(original, original), 1)
        for changed in (original.replace("align=4", "align=8"),
                        original.replace("4 |", "8 |"),
                        original.replace("Length", "MissingField"),
                        original.replace("sizeof=8", "sizeof=16")):
            with self.assertRaisesRegex(ValueError, "Record"):
                windows.verify_layouts(original, changed)
        with self.assertRaisesRegex(ValueError, "no shared"):
            windows.verify_layouts(original, "")

    def test_provider_layouts_follow_renames_and_require_the_needed_records(self):
        original = ("*** Dumping AST Record Layout\n"
                    "         0 | struct URL\n"
                    "         0 |   unsigned short * Text\n"
                    "           | [sizeof=8, align=8]\n"
                    "*** Dumping AST Record Layout\n"
                    "         0 | struct Unused\n"
                    "         0 |   int Value\n"
                    "           | [sizeof=4, align=4]\n")
        generated = original.replace("struct URL", "struct Wide_URL")
        renamed = {"URL": "Wide_URL"}
        self.assertEqual(windows.verify_layouts(original, generated, renamed, ["URL"]), 1)
        self.assertEqual(windows.verify_layouts(original, generated, renamed, ["COUNT"]), 0)
        with self.assertRaisesRegex(ValueError, "missing: URL -> Wide_URL"):
            windows.verify_layouts(original, original, renamed, ["URL"])
        with self.assertRaisesRegex(ValueError, "layout changed: URL"):
            windows.verify_layouts(original, generated.replace("align=8", "align=4"),
                                   renamed, ["URL"])

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
