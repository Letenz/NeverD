// Windows: the window keeps the system frame, which draws its edges and
// shadow and snaps, animates and resizes it, and loses only the caption, as
// Visual Studio Code's custom title bar does there.

#include "WindowFramePlatform.h"

#include <QGuiApplication>
#include <QWindow>
#include <qpa/qplatformwindow_p.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace neverd::gui::platform {
namespace {
constexpr char WindowsPlatform[] = "windows";
/// Visual Studio Code's custom title bar with its command center.
constexpr int TitleBarHeight = 35;

/// Whether windows are the Windows platform plugin's, whose native handles
/// are HWNDs.
bool onWindows() {
  return QGuiApplication::platformName() == QLatin1String(WindowsPlatform);
}

/// The frame the system gives \p window's styles at its DPI, in device
/// pixels: negative left and top, positive right and bottom.  None off the
/// Windows platform plugin.
RECT systemFrame(QWindow &window) {
  RECT frame{};
  if (!onWindows())
    return frame;
  const auto hwnd = reinterpret_cast<HWND>(window.winId());
  AdjustWindowRectExForDpi(
      &frame, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
      static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)),
      GetDpiForWindow(hwnd));
  return frame;
}
} // namespace

FrameKind frameKind() {
  return onWindows() ? FrameKind::Captionless : FrameKind::System;
}

bool drawsShadow() { return false; }

void setShadowMargins(QWindow &, const QMargins &) {}

void removeCaption(QWindow &window) {
  auto *windows =
      window.nativeInterface<QNativeInterface::Private::QWindowsWindow>();
  if (!windows)
    return;
  // The client area grows over the caption up to the frame's top edge; the
  // window then resizes from that edge itself.
  windows->setCustomMargins(QMargins(0, systemFrame(window).top, 0, 0));
}

QMargins maximizedInsets(QWindow &window) {
  // A maximized window's border lies past the screen's top edge.
  const double border = -systemFrame(window).left / window.devicePixelRatio();
  return QMargins(0, qRound(border), 0, 0);
}

// Off the Windows platform plugin no system frame resizes the window.
Qt::Edges ownResizeEdges() {
  return onWindows()
             ? Qt::Edges(Qt::TopEdge)
             : Qt::LeftEdge | Qt::TopEdge | Qt::RightEdge | Qt::BottomEdge;
}

int titleBarHeight() { return TitleBarHeight; }

int systemControlsWidth() { return 0; }

bool drawsWindowControls() { return true; }

bool showsWindowIcon() { return true; }

bool doubleClickTitleBar(QWindow &) { return false; }

} // namespace neverd::gui::platform
