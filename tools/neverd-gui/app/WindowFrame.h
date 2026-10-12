#pragma once

#include <QMargins>
#include <QObject>
#include <QPointer>

class QApplication;
class QWidget;
class QWindow;

namespace neverd::gui {

/// The frame of a top-level window that draws its own, as Visual Studio
/// Code's windows do on Linux: the window manager adds none, a TitleBar
/// inside the window takes the place of its title bar, and the window resizes
/// from every edge and corner.  Under Wayland, where GNOME draws no shadow
/// for such a window, the window also draws a soft shadow outside its visible
/// edge, and that band resizes it too.  Windows keep the window manager's
/// frame on other platforms and when the setting ui/nativeTitleBar is on.
class WindowFrame final : public QObject {
public:
  /// Whether top-level windows draw their own frame.
  static bool enabled();

  /// Give \p window its own frame.  Call before the window first shows, as
  /// its native window takes the frame when it is created.  A \p titleBar,
  /// which the frame then places across the window's top, is for a window
  /// whose layout leaves no room for one.  Returns null where windows keep
  /// the window manager's frame.
  static WindowFrame *attach(QWidget &window, QWidget *titleBar = nullptr);

  /// Give every dialog its own frame and a TitleBar as it first shows,
  /// except popups and dialogs that are frameless already.
  static void frameDialogs(QApplication &app);

  /// The edges a press at \p at resizes in a window of \p size whose visible
  /// part lies \p shadow inside it.  Within a corner's reach a press resizes
  /// both edges.
  static Qt::Edges edgesAt(QPoint at, QSize size, QMargins shadow);

  ~WindowFrame() override;

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  WindowFrame(QWidget &window, QWindow &handle, QWidget *titleBar,
              bool drawsShadow);
  bool resizable() const;
  /// Draw the shadow outside the visible window and its surface inside.
  void paint();
  /// Give the shadow band its size, which a maximized or full-screen window
  /// does without, and keep room for the title bar.
  void updateMargins();
  void placeTitleBar();
  void showEdges(Qt::Edges edges);

  QWidget &window_;
  QWindow &handle_;
  QPointer<QWidget> titleBar_;
  const bool drawsShadow_;
  /// The window's own contents margins, inside the frame's.
  const QMargins content_;
  QMargins margins_;
  Qt::Edges shown_;
};

} // namespace neverd::gui
