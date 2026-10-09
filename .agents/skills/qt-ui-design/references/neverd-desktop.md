# NeverD desktop design profile

Use this profile for NeverD's Qt desktop UI. It captures decisions established
through the Quick Start iterations, including user feedback and actual Qt
renders. These are product defaults, not a universal definition of good taste.
Carry forward choices the user has accepted; change them when new evidence or
an explicit request warrants it.

## Visual direction

Aim for a calm, precise desktop analysis tool with clear, readable content.
Build refinement through proportion, alignment, typography, and coherent states.

- Use solid neutral surfaces with small differences between adjacent regions.
  Decorative gradients, logo halos, edge fades, and floating-card shadows do
  not belong in the established Quick Start direction. Preserve approved brand
  artwork; a logo's own colors are distinct from effects added around it.
- Keep a clear accent. Making every icon desaturated gray made the screen feel
  lifeless in user review. The accepted direction uses a readable blue shared
  by action glyphs and file outlines, with a modest emphasis for selection.
  Use semantic roles in `ChromeColors.def`, including light and dark values;
  treat the current palette as a reference, not a reason to recolor other views.
- Identify file formats with labels and shapes. ELF, BIN, and other formats
  do not need separate brightly colored tiles. Missing files retain a disabled
  treatment so the same accent does not imply availability.
- Distinguish selection, hover, press, and keyboard focus. Use a restrained row
  fill for the default action or selected file. A thin, visible neutral keyboard
  focus outline is appropriate; a persistent bright blue frame is too dominant
  for these small action glyphs. Keep focus reachable and perceivable.

## Hierarchy and proportion

Start by identifying the user's task and the order in which information should
be read. Inspect the whole window before adjusting individual colors.

- Give filenames and action titles priority. Quick Start gives the filename a
  full line, then places the folder and last-opened time below it. Metadata
  should not consume the space needed to distinguish two filenames.
- For long binary names, preserve both ends when eliding so suffixes, build
  variants, and extensions remain useful. Provide the full path and time on
  hover. Keep the primary information usable without requiring hover.
- Keep branding subordinate to starting work. Inspect the combined weight of
  the logo, title, tagline, and version; several individually modest elements
  can still dominate as a group. Startup preferences are secondary controls.
- Reuse the desktop's system font and established density. In the verified
  Quick Start, 14 logical pixels for row titles and 12 for supporting text fit
  the task; 16 pixels is not a mandatory minimum for every dense desktop row.
  Evaluate actual readability, contrast, DPI scaling, and larger system fonts.
  Do not force every size onto a mathematical ratio when that damages fit.
- Use a small spacing scale, generally based on 4 logical pixels. Account for
  SVG padding, stroke placement, and optical alignment instead of rounding
  blindly. Align text edges consistently and center icons within their slots.
  Give related title/description pairs less space than separate action groups.

## Icon family

Use NeverD's original SVGs and the icon registry. Match the icon family across
both columns: stroke weight, corner treatment, apparent size, and color roles.
A shared bounding box alone does not guarantee equal visual weight.

Quick Start uses line glyphs drawn on a 24-pixel grid, without individual rounded
background tiles, beside outlined file icons. Keep them crisp at normal and
fractional DPI. File labels must fit their outline, including longer labels
such as MACH, NDDB, and WASM. Follow `IconSet.def` for asset provenance and
palette conventions; do not copy a third-party icon set merely to imitate an IDE.

## Resizing is part of the design

Content-heavy dialogs should support ordinary desktop resizing unless the task
requires fixed geometry. Set a useful initial size and a content-based minimum
instead of locking the whole dialog with `setFixedSize`.

- Prefer native window edges and Qt's resize grip. Let additional width expose
  more content and additional height expose more rows. Stable navigation or
  action panes can retain their width while the content pane expands.
- Remember a user's chosen size where useful. Bound restored dimensions to
  the available screen, and save normal geometry when closing a maximized
  window. Avoid persisting a size that makes controls unreachable.
- Check the minimum, initial, and enlarged sizes. Text may elide; controls,
  shortcut hints, and hit targets must remain usable. Drag overlays, empty
  states, and keyboard focus must track the resized viewport.

## Verify rendered behavior

Use the real Qt implementation, fonts, and theme. A browser mockup or source
diff does not establish the appearance of a Qt widget.

1. Build the affected GUI targets and inspect real captures in both themes at
   100% and a fractional scale such as 150%. Check long names, empty content,
   disabled actions, selection, hover, and keyboard focus as relevant.
2. For resizing changes, exercise a mouse gesture, content expansion, minimum
   size, and reopening. Use isolated settings so previews and tests do not
   replace the user's preferences or recent-file history.
3. Inspect actual text and widget geometry. Integer font-metric rounding can
   cause a needless ellipsis; verify the final date or shortcut rather than
   assuming its measured width fits. Scrollbars may already be polished before
   an object name is assigned; repolish a scoped widget if its QSS rule does
   not take effect, and confirm the result in pixels.
4. Use existing theme/icon checks and focused interaction regressions. Add a
   test for observable behavior such as drag resizing, not for a preferred
   color literal or a screenshot's exact pixel arrangement.

When reporting an audit, connect each finding to its consequence and proposed
change: for example, a timestamp beside the filename hides a build suffix, so
move metadata below it. Separate observable defects from aesthetic judgment.
Show the actual result and state what was verified. Avoid promising objectively
perfect aesthetics or using decorative effects as a substitute for hierarchy.

## References and implementation

- [NN/g: visual design principles](https://www.nngroup.com/articles/principles-visual-design/)
  — scale, hierarchy, balance, contrast, and grouping.
- [Fluent 2: layout](https://fluent2.microsoft.design/layout)
  — spacing, optical alignment, and responsive content allocation.
- [Microsoft: alignment, margins, and padding](https://learn.microsoft.com/en-us/windows/apps/develop/ui/alignment-margin-padding)
  — fluid layout with deliberate minimum sizes and consistent gutters.
- [Microsoft: icon design](https://learn.microsoft.com/en-us/visualstudio/extensibility/ux-guidelines/images-and-icons-for-visual-studio#style-details)
  — simple metaphors and consistent visual weight across a family.
- [Qt: QDialog resize grip](https://doc.qt.io/qt-6/qdialog.html#sizeGripEnabled-prop).
- Local examples: [QuickStartDialog.cpp](../../../../tools/neverd-gui/app/QuickStartDialog.cpp),
  [ChromeColors.def](../../../../tools/neverd-gui/app/ChromeColors.def),
  [IconSet.def](../../../../tools/neverd-gui/app/IconSet.def), and
  [GUI testing guidance](../../../../docs/testing.md#gui-function-lists-and-navigation).
