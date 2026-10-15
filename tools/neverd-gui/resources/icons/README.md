These workbench icons are original artwork authored for NeverD and are licensed
under the project's license (AGPL-3.0-only); they are not derived from any other
product's icon set. Each icon is a hand-written SVG on a 16 px grid with 1 px
strokes aligned to pixel centers, so it renders crisply at 16 px and scales
cleanly to 24 and 32 px. The set is two-tone: a dark neutral outline (`#3B4250`)
plus one accent color per category (file and pseudocode transformations,
navigation, search, views, lists/windows, edit, analysis, settings), with a
light tint of that accent for body areas so every icon stays legible on both
light and dark toolbars.

Message box icons (`message_information`, `message_warning`,
`message_critical`) take the accent that reads as their severity, the
navigation, file and analysis accents in turn; a question takes `help`, which
with `about` takes the information blue too.

A command that varies another keeps its icon and marks the difference, so the
two stay apart in one menu: a repeat adds the blue repeat badge in the lower
right (`palette_repeat`, `comment_repeatable`), a move to the next or previous
item adds an arrow (`function_next`, `search_bytes_next`), and one level of a
stack marks its layer (`ir_low`, `ir_med`, `ir_high`). A submenu shows the
icon of what its commands have in common (`NEVERD_MENU_ICON` in
`app/Menus.def`).

The chrome glyphs that the theme style sheet draws (`check`, `check_partial`,
`radio_dot`, chevrons, toolbar grips and the menu check mark) are single-color
line shapes. The white ones sit on the accent fill of a checked control; the
others come in `-dark` and `-light` variants named after the theme they are
drawn in, which the style sheet picks with its Mode placeholder.

`app/IconSet.def` is the record of the set: every icon by the group of
commands it serves and what it shows, and every color the set draws with. The
workbench unit tests fail when an icon file is not in the record or a recorded
icon has no file, when an icon leaves the 16 px grid (a chrome glyph takes the
size of the control part it draws) or paints a color the record does not list,
and when one embeds a raster image, text, an external reference, a script or
an editor's metadata. `scripts/check_gui_icons.py`, run by the GUI workflow,
fails when a horizontal or vertical stroke or an unstroked fill edge falls
between two pixels at the icon's viewBox size, where it would draw blurred and
darker than its color; `NEVERD_ICON_SOFT_LINE` in the record names the icons
allowed one, with the reason. A new icon is drawn by hand in the same style
and recorded there with its group. The window icon is the NeverD logo from
`docs/assets`, which keeps its own colors.
