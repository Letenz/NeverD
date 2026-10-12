#include "WindowFrame.h"

#include "SettingsKeys.h"
#include "Theme.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QWidget>
#include <QWindow>
#include <qpa/qplatformwindow_p.h>

namespace neverd::gui {
namespace {
constexpr char WaylandPlatform[] = "wayland";
constexpr char X11Platform[] = "xcb";
/// The style sheet keeps a shadowed window's background clear.
constexpr char DrawsShadowProperty[] = "drawsShadow";
/// How far inside the visible edge a press still resizes the window.
constexpr int InnerEdge = 4;
/// How far along an edge from a corner a press resizes both edges.
constexpr int CornerReach = 16;
/// The band outside the visible window that holds its shadow.
constexpr int ShadowExtent = 10;

bool onWayland() {
  return QGuiApplication::platformName().startsWith(
      QLatin1String(WaylandPlatform));
}

Qt::CursorShape shapeFor(Qt::Edges edges) {
  const bool horizontal = edges & (Qt::LeftEdge | Qt::RightEdge);
  const bool vertical = edges & (Qt::TopEdge | Qt::BottomEdge);
  if (!vertical)
    return Qt::SizeHorCursor;
  if (!horizontal)
    return Qt::SizeVerCursor;
  // Top left and bottom right resize along the falling diagonal.
  const bool left = edges & Qt::LeftEdge;
  const bool top = edges & Qt::TopEdge;
  return left == top ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
}
} // namespace

bool WindowFrame::enabled() {
  if (!onWayland() &&
      QGuiApplication::platformName() != QLatin1String(X11Platform))
    return false;
  return !QSettings().value(settings::NativeTitleBar).toBool();
}

WindowFrame *WindowFrame::attach(QWidget &window) {
  if (!enabled())
    return nullptr;
  const bool drawsShadow = onWayland();
  window.setWindowFlag(Qt::FramelessWindowHint);
  if (drawsShadow) {
    window.setAttribute(Qt::WA_TranslucentBackground);
    window.setProperty(DrawsShadowProperty, true);
  }
  // The native window exists from here, so its margins can be set before
  // the compositor first sizes it.
  window.winId();
  QWindow *handle = window.windowHandle();
  if (!handle)
    return nullptr;
  return new WindowFrame(window, *handle, drawsShadow);
}

WindowFrame::WindowFrame(QWidget &window, QWindow &handle, bool drawsShadow)
    : QObject(&window), window_(window), handle_(handle),
      drawsShadow_(drawsShadow) {
  window.installEventFilter(this);
  handle.installEventFilter(this);
  updateMargins();
}

WindowFrame::~WindowFrame() { showEdges({}); }

Qt::Edges WindowFrame::edgesAt(QPoint at, QSize size, QMargins shadow) {
  const QRect window(QPoint(), size);
  const QRect visible = window.marginsRemoved(shadow);
  const QRect inner =
      visible.adjusted(InnerEdge, InnerEdge, -InnerEdge, -InnerEdge);
  if (!window.contains(at) || inner.contains(at))
    return {};
  Qt::Edges edges;
  if (at.x() < inner.left())
    edges |= Qt::LeftEdge;
  else if (at.x() > inner.right())
    edges |= Qt::RightEdge;
  if (at.y() < inner.top())
    edges |= Qt::TopEdge;
  else if (at.y() > inner.bottom())
    edges |= Qt::BottomEdge;
  if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
    if (at.y() < visible.top() + CornerReach)
      edges |= Qt::TopEdge;
    else if (at.y() > visible.bottom() - CornerReach)
      edges |= Qt::BottomEdge;
  }
  if (edges & (Qt::TopEdge | Qt::BottomEdge)) {
    if (at.x() < visible.left() + CornerReach)
      edges |= Qt::LeftEdge;
    else if (at.x() > visible.right() - CornerReach)
      edges |= Qt::RightEdge;
  }
  return edges;
}

bool WindowFrame::resizable() const {
  if (window_.windowState() &
      (Qt::WindowMaximized | Qt::WindowFullScreen | Qt::WindowMinimized))
    return false;
  return window_.minimumSize() != window_.maximumSize();
}

bool WindowFrame::eventFilter(QObject *object, QEvent *event) {
  if (object == &window_) {
    if (event->type() == QEvent::WindowStateChange)
      updateMargins();
    else if (event->type() == QEvent::Paint && drawsShadow_)
      paint();
    return false;
  }
  if (object != &handle_)
    return false;
  if (event->type() == QEvent::Leave) {
    showEdges({});
    return false;
  }
  if (event->type() != QEvent::MouseMove &&
      event->type() != QEvent::MouseButtonPress)
    return false;
  auto *mouse = static_cast<QMouseEvent *>(event);
  const Qt::Edges edges = resizable() ? edgesAt(mouse->position().toPoint(),
                                                window_.size(), margins_)
                                      : Qt::Edges();
  if (event->type() == QEvent::MouseMove) {
    if (mouse->buttons() == Qt::NoButton)
      showEdges(edges);
    return false;
  }
  if (mouse->button() != Qt::LeftButton || !edges)
    return false;
  showEdges({});
  return handle_.startSystemResize(edges);
}

void WindowFrame::paint() {
  const auto &theme = Theme::instance();
  const QRect visible = window_.rect().marginsRemoved(margins_);
  QPainter painter(&window_);
  if (drawsShadow_)
    painter.fillRect(visible, theme.chrome(QStringLiteral("Window")));
  if (margins_.isNull())
    return;
  // Rings fading away from the edge, then the edge's hairline over them.
  const QColor shadow = theme.chrome(QStringLiteral("WindowShadow"));
  const QRectF edge = QRectF(visible).adjusted(-0.5, -0.5, 0.5, 0.5);
  for (int ring = ShadowExtent; ring >= 1; --ring) {
    const double fade = 1.0 - double(ring - 1) / ShadowExtent;
    QColor color = shadow;
    color.setAlphaF(shadow.alphaF() * fade * fade);
    painter.setPen(QPen(color, 1));
    painter.drawRect(edge.adjusted(-ring, -ring, ring, ring));
  }
  painter.setPen(QPen(theme.chrome(QStringLiteral("WindowEdge")), 1));
  painter.drawRect(edge);
}

void WindowFrame::updateMargins() {
  const bool framed =
      !(window_.windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen));
  const QMargins margins =
      drawsShadow_ && framed
          ? QMargins(ShadowExtent, ShadowExtent, ShadowExtent, ShadowExtent)
          : QMargins();
  if (margins == margins_)
    return;
  margins_ = margins;
#if QT_CONFIG(wayland)
  // The compositor sizes, places and tiles the visible window, not its
  // shadow.
  if (auto *wayland =
          handle_.nativeInterface<QNativeInterface::Private::QWaylandWindow>())
    wayland->setCustomMargins(margins);
#endif
  window_.setContentsMargins(margins);
  window_.update();
}

void WindowFrame::showEdges(Qt::Edges edges) {
  if (edges == shown_)
    return;
  if (shown_)
    QGuiApplication::restoreOverrideCursor();
  if (edges)
    QGuiApplication::setOverrideCursor(shapeFor(edges));
  shown_ = edges;
}

} // namespace neverd::gui
