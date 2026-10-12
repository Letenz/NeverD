#pragma once

#include <QPointer>
#include <QWidget>
#include <optional>

class QAbstractButton;
class QAction;
class QMenuBar;

namespace neverd::gui {

/// The title bar of a window that draws its own frame (WindowFrame), after
/// Visual Studio Code's custom title bar: the application icon, the window's
/// menus, a command center that names the open file and runs a command, and
/// the minimize, maximize and close buttons.  Dragging its empty parts moves
/// the window and double-clicking them maximizes or restores it.  It dims
/// while the window is inactive; full screen hides the icon and the buttons.
class TitleBar final : public QWidget {
  Q_OBJECT
public:
  /// A bar at the top of \p window that shows \p menuBar and takes it.
  TitleBar(QWidget &window, QMenuBar &menuBar);

  /// Show \p text in the command center, with \p toolTip over it.
  void setCommandCenterText(const QString &text, const QString &toolTip);
  /// The command the command center runs when clicked.
  void setCommandCenterAction(QAction *action);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

protected:
  bool event(QEvent *event) override;
  bool eventFilter(QObject *object, QEvent *event) override;
  void changeEvent(QEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;

private:
  /// Place the menus at the left, the buttons at the right and the command
  /// center in the middle of the bar, or in the room the menus leave.
  void arrange();
  void toggleMaximized();
  /// Follow the window into and out of the maximized and full-screen states.
  void updateWindowState();
  /// Dim the bar while the window is inactive.
  void updateActive();
  void retranslateUi();
  void repaintAll();

  QWidget &window_;
  QMenuBar &menuBar_;
  QAbstractButton *commandCenter_;
  QAbstractButton *minimize_;
  QAbstractButton *maximize_;
  QAbstractButton *close_;
  QPointer<QAction> command_;
  /// Where a press on the bar's empty part began a possible window move.
  std::optional<QPoint> pressAt_;
};

} // namespace neverd::gui
