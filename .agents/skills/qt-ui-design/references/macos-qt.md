# macOS Qt rendering and input

Use these checks when implementing or verifying Qt desktop UI on macOS. They
come from failures reproduced with NeverD's Qt Widgets workbench on Cocoa;
apply NeverD's appearance choices only when working on NeverD.

## Menus and icons

Qt 6.7.3 changed the macOS default of `Qt::AA_DontShowIconsInMenus` to true.
`QAction::setIcon()` can therefore load a valid icon that appears in a toolbar
but disappears in menus. This also affects dynamically created context menus.
For NeverD's application-painted menus, `Theme::apply()` clears this attribute
after `QApplication` has initialized platform defaults. Native-menu ownership
and icon visibility are separate choices; changing `setNativeMenuBar()` alone
does not restore icons.

Verify both `action->icon().pixmap(...).isNull()` and
`action->isIconVisibleInMenu()`, then inspect an actual menu. Include a dynamic
context menu such as Output's Copy/Select all/Clear, not just registry actions.
If icons are actually missing from a packaged application, inspect its resource
registration and SVG runtime separately from the visibility policy.

## Precision scrolling and zoom

A wheel notch is 120 angle units, but individual events may be smaller.
Trackpads also report `pixelDelta()`, including horizontal movement. Prefer
pixel deltas when present, retain partial rows and notches between events,
and avoid counting the pixel and angle versions of the same event twice.
An event sequence and its combined movement should scroll the same distance.
For text rows, keep integer remainders before division; per-event truncation
loses slow gestures. NeverD shares this behavior in `app/WheelInput`.

`ScrollBegin` and `ScrollEnd` can have no movement. Zero vertical delta must
not shrink a font, and horizontal scrolling with the zoom modifier must not
change its size. Accumulate precision zoom deltas too, rather than changing
one font point for every tiny event. Reset unconsumed input at gesture
boundaries and when switching between scrolling and zooming.

Qt maps portable Control shortcuts to Command on macOS. Keep shortcut storage
in `QKeySequence::PortableText`, show hints using `NativeText`, and verify the
actual event modifiers before adding platform-specific key swaps.

## Cocoa and Retina validation

- Run focused tests with `QT_QPA_PLATFORM=cocoa` as well as offscreen. Use
  isolated `QSettings`, and exercise both themes and fractional scaling.
- A captured image stores physical pixels. Convert widget-local logical
  points to image coordinates with the capture's `devicePixelRatio()` before
  sampling colors; otherwise a Retina check can sample the neighboring pane
  or dialog background and misreport correct rendering as broken.
- Apply the workbench theme before testing widget geometry. macOS's native
  style can give `QWidgetItem` layout margins that differ from a widget's
  `sizeHint()`; a test using the native style does not establish how NeverD's
  Fusion-based style lays out the same control.
- Check `CGPreflightPostEventAccess()` before tests post native mouse events.
  `CGEventPost()` returning without an error does not establish delivery.
  Skip and report unavailable native-pointer coverage when permission is
  absent; do not classify those missed clicks as docking product defects.
- Observe AppKit completion when testing full-screen transitions. An eagerly
  updated Qt window-state flag is insufficient; NeverD's
  `tests/platform/NativeWindowStateMac.mm` provides that observation. First
  establish an active foreground test window. If macOS refuses activation,
  report native full-screen coverage as unavailable instead of interpreting
  the missing transition as a product regression.

## Development bundles

Keep the GUI, worker, MCP host and engine from the same source build. In an
application bundle the GUI finds its worker beside itself in `Contents/MacOS`.
Use NeverD's `package_macos.py` helper to deploy the matching Qt kit, repair
dependencies, audit the bundle and preserve engine-host entitlements while
signing. Check the packaged executable's startup and the bundled worker,
as well as the unbundled build. The normal package contains Cocoa's platform
plugin; offscreen tests run against the build tree, unless that plugin is
explicitly deployed for a separate test package.

References: [Qt 6.7.3 menu-default change](https://code.qt.io/cgit/qt/qtreleasenotes.git/about/qt/6.7.3/release-note.md),
[QAction icon visibility](https://doc.qt.io/qt-6/qaction.html#iconVisibleInMenu-prop),
[QWheelEvent deltas and phases](https://doc.qt.io/qt-6/qwheelevent.html).
