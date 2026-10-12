#pragma once

class QWidget;

namespace neverd::gui::gnome {

/// GNOME draws no frame for a Wayland window: Qt draws it, and resizes the
/// window only from a few pixels outside its visible edge.  This lets the
/// last pixels inside the left, right and bottom edges of \p window resize
/// it too, under the matching cursor, as GNOME's own windows allow.
/// Elsewhere it does nothing: the window manager's frame resizes windows.
void widenResizeEdges(QWidget &window);

/// QtWayland tells the compositor a dimension has no maximum only when Qt's
/// frame adds nothing to it.  A maximized window's frame has a title bar but
/// no side borders, so its width goes out unbounded and its height bounded,
/// and GNOME reads that pair as a maximum width of zero: once restored, the
/// window no longer resizes sideways and a vertical resize narrows it to its
/// minimum width.  This gives \p window a finite maximum larger than any
/// screen in both dimensions.  Elsewhere it does nothing.
void boundMaximumSize(QWidget &window);

} // namespace neverd::gui::gnome
