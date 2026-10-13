// Windows: the window keeps the system frame, which draws its edges and
// shadow and snaps, animates and resizes it, and loses only the caption, as
// Visual Studio Code's custom title bar does there.

#include "WindowFramePlatform.h"

#include <QAbstractNativeEventFilter>
#include <QGuiApplication>
#include <QHash>
#include <QPointer>
#include <QWindow>
#include <qpa/qplatformwindow_p.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace neverd::gui::platform {
namespace {
constexpr char WindowsPlatform[] = "windows";
/// The type of the messages Qt's Windows platform plugin filters.
constexpr char WindowsMessage[] = "windows_generic_MSG";
/// Visual Studio Code's custom title bar with its command center.
constexpr int TitleBarHeight = 35;

/// Qt applies a window's custom margins to every WM_NCCALCSIZE.  The margins
/// that take a captionless window's client area over its caption push it
/// past the screen's top when the window has no caption, which is how Qt
/// makes a window full screen (WS_POPUP).  Such a window takes the system's
/// client area, as Chromium's custom frames do; Qt reads the margins back
/// from the result.  Only windows given these margins are handled: others,
/// such as KDDockWidgets' floating windows, size their own client areas.
class CaptionMarginFilter final : public QAbstractNativeEventFilter {
public:
  static CaptionMarginFilter &instance() {
    static auto *filter = [] {
      auto *created = new CaptionMarginFilter;
      QCoreApplication::instance()->installNativeEventFilter(created);
      return created;
    }();
    return *filter;
  }

  void add(QWindow &window) {
    windows_.insert(reinterpret_cast<HWND>(window.winId()), &window);
  }

  bool nativeEventFilter(const QByteArray &type, void *message,
                         qintptr *result) override {
    if (type != WindowsMessage)
      return false;
    const auto *msg = static_cast<const MSG *>(message);
    if (msg->message != WM_NCCALCSIZE || !msg->wParam || !owns(msg->hwnd))
      return false;
    const LONG_PTR style = GetWindowLongPtrW(msg->hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION)
      return false;
    *result = DefWindowProcW(msg->hwnd, msg->message, msg->wParam, msg->lParam);
    return true;
  }

private:
  /// Whether \p hwnd is still the native window of a window given margins;
  /// a handle can return for another window once its own is destroyed.
  bool owns(HWND hwnd) {
    const auto found = windows_.constFind(hwnd);
    if (found == windows_.cend())
      return false;
    const QWindow *window = found->data();
    if (window && window->handle() &&
        reinterpret_cast<HWND>(window->winId()) == hwnd)
      return true;
    windows_.erase(found);
    return false;
  }

  QHash<HWND, QPointer<QWindow>> windows_;
};

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
  CaptionMarginFilter::instance().add(window);
  // A full-screen window has no caption to measure.  It keeps the margins of
  // its captioned style, which Qt rescales as the window changes screens.
  const auto hwnd = reinterpret_cast<HWND>(window.winId());
  if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION) != WS_CAPTION)
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
