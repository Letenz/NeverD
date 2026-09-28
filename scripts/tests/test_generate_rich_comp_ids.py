import unittest

from scripts.generate_rich_comp_ids import records, release_year, render

SAMPLE = """\
# Format is:
# <comp.id> Description

0102 [LNK] VS2015+ (14.0+)        # prodidLinker1400
0083 [ C ] VS2008 (9.0)           # prodidUtc1500_C
01028da1 [LNK] VS2026 v18.10.0 build 36257
01068da4 [CIL] VS2026 v18.10.2 build 36260 (*)
00837809 [ C ] VS2008 SP1 build 30729
"""


class RichCompIdTests(unittest.TestCase):
    def test_product_and_build_records_are_kept_apart(self):
        products, builds = records(SAMPLE)
        self.assertEqual(products[0x0102], ("Linker", 2015, False, "VS2015+ (14.0+)"))
        self.assertEqual(products[0x0083], ("C", 2008, False, "VS2008 (9.0)"))
        self.assertEqual(builds[(0x0102, 36257)],
                         ("Linker", 2026, False, "VS2026 v18.10.0 build 36257"))
        self.assertEqual(builds[(0x0106, 36260)],
                         ("CvtcilC", 2026, True, "VS2026 v18.10.2 build 36260"))

    def test_release_years(self):
        self.assertEqual(release_year("VS2026 v18.10.0 build 36257"), 2026)
        self.assertEqual(release_year("VS2015+ (14.0+)"), 2015)
        self.assertEqual(release_year("VS98 (6.0) SP6 build 8447"), 1998)
        self.assertEqual(release_year("VS6 build 8168"), 1998)
        self.assertEqual(release_year("Phoenix (10.0)"), 0)

    def test_malformed_input_is_rejected(self):
        for text in ("0102 [LNK] VS2015\n0102 [LNK] VS2015\n",
                     "0102 [XYZ] VS2015\n",
                     "not a record\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                records(text)

    def test_rendering_is_sorted_and_deterministic(self):
        products, builds = records(SAMPLE)
        text = render(products, builds, "0" * 64)
        self.assertEqual(text, render(*records(SAMPLE), "0" * 64))
        self.assertLess(text.index("{0x0083, RichTool::C"), text.index("{0x0102, RichTool::Linker"))
        self.assertIn('{0x0102, 36257, RichTool::Linker, 2026, false, "VS2026 v18.10.0 build 36257"},',
                      text)
        self.assertIn("LICENSES/richprint.txt", text)


if __name__ == "__main__":
    unittest.main()
