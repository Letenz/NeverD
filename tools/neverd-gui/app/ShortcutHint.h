//===- ShortcutHint.h - Visible keyboard hints on buttons ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Show a button's actual key beside its icon or label without registering
/// another shortcut or changing the scope of the existing binding.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_SHORTCUT_HINT_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_SHORTCUT_HINT_H

#include <QAbstractButton>
#include <QKeySequence>
#include <QToolButton>

namespace neverd::gui {

inline void setShortcutHint(QAbstractButton &button, const QKeySequence &key) {
  const QString hint = key.toString(QKeySequence::NativeText);
  if (auto *tool = qobject_cast<QToolButton *>(&button)) {
    tool->setToolButtonStyle(hint.isEmpty() ? Qt::ToolButtonIconOnly
                                            : Qt::ToolButtonTextBesideIcon);
    if (!hint.isEmpty())
      tool->setText(hint);
    // The visible key must not replace the command's accessible name.
    tool->setAccessibleName(tool->toolTip());
  } else if (!hint.isEmpty()) {
    button.setText(QStringLiteral("%1 (%2)").arg(button.text(), hint));
  }
}

} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_SHORTCUT_HINT_H
