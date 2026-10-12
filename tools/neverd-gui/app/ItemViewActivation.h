#pragma once

class QAbstractItemView;

namespace neverd::gui {

/// Let Return and Enter open \p view's current item on every system, as the
/// classic list windows do.  macOS item views start editing the item instead
/// and never report it activated, so Enter in a chooser did nothing there.
void activateOnReturn(QAbstractItemView &view);

} // namespace neverd::gui
