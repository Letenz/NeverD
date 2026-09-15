from pathlib import Path
import tempfile
import unittest

from scripts.generate_objc_framework_declarations import framework_header, module_paths, owns_header, render


class ObjCFrameworkDeclarationTests(unittest.TestCase):
    def test_public_umbrella_retains_provider_boundary(self):
        self.assertEqual(framework_header("QuartzCore"), "QuartzCore/CoreAnimation.h")
        self.assertEqual(framework_header("CoreLocation"), "CoreLocation/CoreLocation.h")
        for name in ("../CoreData", "QuartzCore/CoreImage", "QuartzCore>\n#import <Other"):
            with self.assertRaises(ValueError):
                framework_header(name)

    def test_headers_belong_to_one_framework(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            headers = root / "First.framework/Headers"
            self.assertTrue(owns_header(headers / "Class.h", headers))
            self.assertFalse(owns_header(root / "Second.framework/Headers/Class.h", headers))
            self.assertFalse(owns_header(root / "First.framework/HeadersExtra/Class.h", headers))
            self.assertFalse(owns_header(headers / "../PrivateHeaders/Class.h", headers))

    def test_install_names_are_exact_public_framework_paths(self):
        root = "/System/Library/Frameworks/CoreData.framework/"
        self.assertEqual(module_paths("CoreData", root + "Versions/A/CoreData"),
                         root + "CoreData|" + root + "Versions/A/CoreData")
        for path in ["/tmp/CoreData.framework/CoreData", root + "Other", root + "Versions/A/Other"]:
            with self.assertRaises(ValueError):
                module_paths("CoreData", path)
        with self.assertRaises(ValueError):
            module_paths("../Other", root + "CoreData")

    def test_frameworks_keep_independent_negative_and_missing_facts(self):
        fixed = {"value": {"Q16@0:8"}}
        unsupported = {"value": {""}}
        text = render([
            ("B", "b", [unsupported] * 4),
            ("A", "a", [fixed] * 4),
            ("C", "c", [fixed, {}, fixed, {}]),
        ], "test", "test")
        self.assertIn('{"A", "a", "value", "Q16@0:8", "Q16@0:8"}', text)
        self.assertIn('{"B", "b", "value", "", ""}', text)
        self.assertNotIn('{"C",', text)
        self.assertLess(text.index('{"A",'), text.index('{"B",'))


if __name__ == "__main__":
    unittest.main()
