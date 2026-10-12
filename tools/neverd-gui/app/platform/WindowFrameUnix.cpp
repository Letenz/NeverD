// Linux and other X11 or Wayland desktops: the window drops the window
// manager's frame and draws its own, with a shadow under Wayland.

#include "WindowFramePlatform.h"

#include <QGuiApplication>
#include <QWindow>
#include <qpa/qplatformwindow_p.h>

namespace neverd::gui::platform {
namespace {
constexpr char WaylandPlatform[] = "wayland";
constexpr char X11Platform[] = "xcb";
/// Visual Studio Code's custom title bar with its command center.
constexpr int TitleBarHeight = 35;

bool onWayland() {
  return QGuiApplication::platformName().startsWith(
      QLatin1String(WaylandPlatform));
}
} // namespace

FrameKind frameKind() {
  if (onWayland() ||
      QGuiApplication::platformName() == QLatin1String(X11Platform))
    return FrameKind::Frameless;
  return FrameKind::System;
}

bool drawsShadow() { return onWayland(); }

void setShadowMargins(QWindow &window, const QMargins &margins) {
#if QT_CONFIG(wayland)
  // The compositor sizes, places and tiles the visible window, not its
  // shadow.
  if (auto *wayland =
          window.nativeInterface<QNativeInterface::Private::QWaylandWindow>())
    wayland->setCustomMargins(margins);
#else
  Q_UNUSED(window);
  Q_UNUSED(margins);
#endif
}

void removeCaption(QWindow &) {}

QMargins maximizedInsets(QWindow &) { return {}; }

Qt::Edges ownResizeEdges() {
  return Qt::LeftEdge | Qt::TopEdge | Qt::RightEdge | Qt::BottomEdge;
}

int titleBarHeight() { return TitleBarHeight; }

int systemControlsWidth() { return 0; }

bool drawsWindowControls() { return true; }

bool showsWindowIcon() { return true; }

bool doubleClickTitleBar(QWindow &) { return false; }

} // namespace neverd::gui::platform
