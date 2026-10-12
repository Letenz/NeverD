#pragma once

#include <QLabel>
#include <QSizePolicy>
#include <QWidget>

namespace neverd::gui {

/// Lets a row of a docked view, such as its status line or a bar of buttons,
/// be narrower than its content.  The narrowest a dock can be is the widest
/// row of its view, and a dock that cannot narrow stops the separators beside
/// it: the disassembly's status ("0000000000003420: start (Synchronized with
/// Hex View-1)") held its dock over 500 pixels wide.  What does not fit is cut
/// at the right edge.
inline void makeRowShrinkable(QWidget &row) {
  row.setSizePolicy(QSizePolicy::Ignored, row.sizePolicy().verticalPolicy());
}

/// Makes \p label the status line under a docked view: a shrinkable row that
/// the theme style sheet draws in the secondary text color, so the position it
/// describes stays quieter than the view itself.
inline void makeStatusLine(QLabel &label) {
  label.setContentsMargins(6, 2, 6, 2);
  label.setProperty("statusLine", true);
  makeRowShrinkable(label);
}

} // namespace neverd::gui
