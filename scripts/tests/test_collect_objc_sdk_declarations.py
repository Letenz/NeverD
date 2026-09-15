import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from scripts import collect_objc_sdk_declarations as collector
from scripts.generate_objc_receiver_declarations import ReceiverDeclarations


class ObjCSDKCollectionTests(unittest.TestCase):
    def test_full_plan_keeps_real_device_simulator_catalyst_and_desktop_separate(self):
        plan = collector.profile_plan(collector.FRAMEWORKS, collector.SDKS)
        self.assertEqual(len(plan), 47)
        self.assertEqual(len({row["id"] for row in plan}), 47)
        ui = [row for row in plan if row["framework"] == "UIKit"]
        self.assertEqual(len(ui), 5)
        self.assertNotIn("desktop", {row["environment"] for row in ui})
        self.assertEqual({row["sdk"] for row in ui}, set(collector.SDKS))
        for row in plan:
            if row["environment"] == "device":
                self.assertEqual((row["sdk"], row["target"]),
                                 ("iphoneos", "arm64-apple-ios18.0"))
            elif row["environment"] == "simulator":
                self.assertEqual(row["sdk"], "iphonesimulator")
                self.assertTrue(row["target"].endswith("-simulator"))
            elif row["environment"] == "catalyst":
                self.assertEqual(row["sdk"], "macosx")
                self.assertTrue(row["target"].endswith("-macabi"))

    def test_partial_local_plan_never_claims_an_ios_profile(self):
        plan = collector.profile_plan(["UIKit", "CoreImage"], ["macosx"])
        self.assertEqual(len(plan), 6)
        self.assertEqual({row["environment"] for row in plan}, {"catalyst", "desktop"})
        self.assertEqual(plan, collector.profile_plan(["CoreImage", "UIKit"], ["macosx"]))

    def test_invalid_or_duplicated_selections_fail_before_path_construction(self):
        for frameworks, sdks in [([], ["macosx"]), (["UIKit", "UIKit"], ["macosx"]),
                                 (["UIKit"], []), (["UIKit"], ["macosx", "macosx"]),
                                 (["UIKit"], ["unknown"]), (["../UIKit"], ["macosx"]),
                                 (["UIKit>\n#import <Other"], ["macosx"])]:
            with self.subTest(frameworks=frameworks, sdks=sdks), self.assertRaises(ValueError):
                collector.profile_plan(frameworks, sdks)

    def test_catalyst_uses_its_own_public_headers_without_hiding_missing_desktop(self):
        with tempfile.TemporaryDirectory() as temporary:
            sdk = Path(temporary).resolve()
            desktop = sdk / "System/Library/Frameworks/Probe.framework"
            catalyst = sdk / "System/iOSSupport/System/Library/Frameworks/Probe.framework"
            desktop.mkdir(parents=True)
            self.assertEqual(collector.framework_directory(sdk, "Probe", "catalyst"), desktop)
            catalyst.mkdir(parents=True)
            self.assertEqual(collector.framework_directory(sdk, "Probe", "catalyst"), catalyst)
            self.assertEqual(collector.framework_directory(sdk, "Probe", "desktop"), desktop)
            desktop.rmdir()
            with self.assertRaises(FileNotFoundError):
                collector.framework_directory(sdk, "Probe", "desktop")

    def test_public_framework_cannot_resolve_outside_its_sdk(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            sdk, outside = root / "sdk", root / "outside"
            public = sdk / "System/Library/Frameworks"
            public.mkdir(parents=True)
            outside.mkdir()
            (public / "Probe.framework").symlink_to(outside, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "outside"):
                collector.framework_directory(sdk, "Probe", "desktop")

    def run_fake_collection(self, root, failure=None, version="test"):
        sdk = root / "sdk"
        sdk.mkdir()
        (sdk / "SDKSettings.json").write_text('{"Version":"test"}')
        library = root / "libclang"
        library.write_bytes(b"test compiler")

        def extract(output, sdk, library, profile):
            if failure and profile["target"] == failure:
                raise RuntimeError("compiler rejected an incomplete public import")
            return {**profile, "status": "success", "method_count": 1}

        with patch.object(collector, "collect_profile", side_effect=extract):
            return collector.collect(root / "output", {"macosx": sdk}, library,
                                     ["UIKit"], version)

    def test_last_profile_failure_cannot_publish_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaisesRegex(RuntimeError, "incomplete"):
                self.run_fake_collection(root, "x86_64-apple-ios18.0-macabi")
            result = json.loads((root / "output/receipt.json").read_text())
            self.assertEqual(result["status"], "incomplete")
            self.assertEqual(len(result["expected_profiles"]), 2)
            self.assertEqual(len(result["profiles"]), 1)
            self.assertIn("error", result)

    def test_wrong_sdk_version_fails_before_declaration_extraction(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaisesRegex(ValueError, "expected SDK"):
                self.run_fake_collection(root, version="different")
            result = json.loads((root / "output/receipt.json").read_text())
            self.assertEqual(result["status"], "incomplete")
            self.assertEqual(result["profiles"], [])

    def test_complete_receipt_retains_its_requested_scope(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = self.run_fake_collection(Path(temporary))
            self.assertEqual(result["status"], "success")
            self.assertEqual(result["requested_sdks"], ["macosx"])
            self.assertEqual(result["frameworks"], ["UIKit"])
            self.assertEqual([row["id"] for row in result["profiles"]],
                             [row["id"] for row in result["expected_profiles"]])

    @unittest.skipUnless(sys.platform == "darwin", "Requires the macOS libclang runtime")
    def test_explicit_search_paths_preserve_owned_facts_and_parse_errors(self):
        library = Path("/Library/Developer/CommandLineTools/usr/lib/libclang.dylib")
        if not library.is_file():
            self.skipTest("libclang is unavailable")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            headers = root / "public"
            headers.mkdir()
            header = headers / "Owned.h"
            header.write_text("""
__attribute__((objc_root_class)) @interface Owner
- (instancetype)related;
- (Owner *)peer;
- (id)format:(id)value, ...;
@end
""")
            source = root / "source.m"
            source.write_text("#import <Owned.h>\n")
            compiler = ReceiverDeclarations(library, headers)
            with self.assertRaisesRegex(RuntimeError, "file not found"):
                compiler.extract_owned(source, root, "arm64-apple-macos15.0")
            facts = compiler.extract_owned(source, root, "arm64-apple-macos15.0",
                                           ["-I" + str(headers)])
            methods = {row[4]: row for row in facts["methods"]}
            self.assertTrue(methods["related"][7])
            self.assertEqual(methods["peer"][6], "Owner")
            self.assertEqual(methods["format:"][5], "")
            header.write_text("@interface Broken :\n")
            with self.assertRaises(RuntimeError):
                compiler.extract_owned(source, root, "arm64-apple-macos15.0",
                                       ["-I" + str(headers)])


if __name__ == "__main__":
    unittest.main()
