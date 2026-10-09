#pragma once

class QWidget;

namespace neverd::gui::gnome {

/// GNOME draws no frame for a Wayland window: Qt draws it, and resizes the
/// window only from a few pixels outside its visible edge.  This lets the
/// last pixels inside the left, right and bottom edges of \p window resize
/// it too, under the matching cursor, as GNOME's own windows allow.
/// Elsewhere it does nothing: the window manager's frame resizes windows.
void widenResizeEdges(QWidget &window);

} // namespace neverd::gui::gnome
