from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from scripts import check_gui_icons as icons

HEADER = ('<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" '
          'viewBox="0 0 16 16">')


class CheckGuiIconsTest(unittest.TestCase):
    def setUp(self) -> None:
        self._directory = tempfile.TemporaryDirectory()
        self.root = Path(self._directory.name)

    def tearDown(self) -> None:
        self._directory.cleanup()

    def icon(self, body: str, name: str = "probe", size: int = 16) -> Path:
        path = self.root / f"{name}.svg"
        path.write_text(HEADER.format(size=size) + body + "</svg>", encoding="utf-8")
        return path

    def problems(self, body: str, allowed: set[str] | None = None, **icon) -> list[str]:
        return icons.check_icon(self.icon(body, **icon), allowed or set())

    def test_repository_icons_draw_lines_on_whole_pixels(self) -> None:
        allowed = icons.allowed_icons(icons.RECORD)
        problems = [problem for path in sorted(icons.ICONS.glob("*.svg"))
                    for problem in icons.check_icon(path, allowed)]
        self.assertEqual(problems, [])

    def test_one_pixel_line_must_center_on_a_pixel(self) -> None:
        # The file dialog's list button drew its rows on pixel boundaries.
        self.assertEqual(len(self.problems('<path d="M6 4h8.5M6 8h8.5" stroke="#3B4250"/>')), 2)
        self.assertEqual(self.problems('<path d="M6 4.5h8.5M6 8.5h8.5" stroke="#3B4250"/>'), [])

    def test_two_pixel_line_must_center_on_a_boundary(self) -> None:
        self.assertEqual(self.problems('<path d="M4 7h6" stroke="#FFFFFF" stroke-width="2"/>'), [])
        self.assertEqual(len(self.problems('<path d="M4 7.5h6" stroke="#FFFFFF" stroke-width="2"/>')), 1)

    def test_thin_strokes_take_their_width_from_the_root(self) -> None:
        # The quick start's New glyph before #999: a 1.4 px stroke set on the
        # root, with the page's two sides on pixel boundaries.
        glyph = self.root / "start_new.svg"
        glyph.write_text(
            '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" '
            'viewBox="0 0 24 24" fill="none" stroke="#C5C5C5" stroke-width="1.4">'
            '<path d="M13.5 3.5h-7A1.5 1.5 0 0 0 5 5v14a1.5 1.5 0 0 0 1.5 1.5h11'
            'A1.5 1.5 0 0 0 19 19V9z"/></svg>', encoding="utf-8")
        self.assertEqual(len(icons.check_icon(glyph, set())), 2)

    def test_lines_scale_with_the_size_the_icon_is_drawn_at(self) -> None:
        # A 16 px grid drawn at 32 px doubles every coordinate and stroke.
        self.assertEqual(self.problems('<path d="M6 4h8" stroke="#3B4250"/>', size=32), [])

    def test_fill_edge_between_pixels_needs_a_stroke_over_it(self) -> None:
        # The warning's bar had its top edge inside a pixel row.
        bar = '<path d="M7 5.5h2v4.5H7z" fill="#FFFFFF"/>'
        self.assertEqual(len(self.problems(bar)), 1)
        covered = ('<path d="M2 3h12v2.5H2z" fill="#2E9E8F"/>'
                   '<path d="M1.5 5.5h13" fill="none" stroke="#3B4250"/>')
        self.assertEqual(self.problems(covered), [])

    def test_curves_diagonals_and_corners_are_not_lines(self) -> None:
        body = ('<circle cx="8" cy="8" r="3" stroke="#3B4250"/>'
                '<path d="M3 3l10 10M8 1a7 7 0 0 1 0 14h1" stroke="#3B4250"/>')
        self.assertEqual(self.problems(body), [])

    def test_record_allows_an_icon_its_soft_line(self) -> None:
        ridge = '<path d="M8 8.5v4" stroke="#FFFFFF"/>'
        self.assertEqual(len(self.problems(ridge, name="signatures")), 1)
        self.assertEqual(self.problems(ridge, allowed={"signatures"}, name="signatures"), [])
        self.assertIn("signatures", icons.allowed_icons(icons.RECORD))

    def test_unreadable_geometry_fails_instead_of_passing(self) -> None:
        self.assertEqual(len(self.problems('<path d="M2 2h4" transform="translate(.5 0)" '
                                           'stroke="#3B4250"/>')), 1)
        self.assertEqual(len(self.problems('<path d="M2 2h4z 5" stroke="#3B4250"/>')), 1)

    def test_main_reports_and_fails(self) -> None:
        bad = self.icon('<path d="M6 4h8.5" stroke="#3B4250"/>', name="bad")
        self.assertEqual(icons.main([str(bad), "--record", str(icons.RECORD)]), 1)
        good = self.icon('<path d="M6 4.5h8.5" stroke="#3B4250"/>', name="good")
        self.assertEqual(icons.main([str(good), "--record", str(icons.RECORD)]), 0)


if __name__ == "__main__":
    unittest.main()
