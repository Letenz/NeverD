#include "GnomeWindowEdges.h"

#include "GnomeModalDialogs.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QWidget>
#include <QWindow>

namespace neverd::gui::gnome {
namespace {
constexpr char WaylandPlatform[] = "wayland";
/// How far inside an edge a press still resizes the window.
constexpr int EdgeWidth = 4;

class ResizeEdges final : public QObject {
public:
  ResizeEdges(QWidget &window, QWindow &handle)
      : QObject(&handle), window_(window), handle_(handle) {
    handle.installEventFilter(this);
  }

protected:
  bool eventFilter(QObject *object, QEvent *event) override {
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
    const Qt::Edges edges = edgesAt(mouse->position().toPoint());
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

private:
  /// The edges at \p at, the top one being the title bar's.  A maximized or
  /// full-screen window has none.
  Qt::Edges edgesAt(QPoint at) const {
    Qt::Edges edges;
    if (window_.windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))
      return edges;
    if (at.x() < EdgeWidth)
      edges |= Qt::LeftEdge;
    if (at.x() >= window_.width() - EdgeWidth)
      edges |= Qt::RightEdge;
    if (at.y() >= window_.height() - EdgeWidth)
      edges |= Qt::BottomEdge;
    return edges;
  }

  static Qt::CursorShape shapeFor(Qt::Edges edges) {
    const bool horizontal = edges & (Qt::LeftEdge | Qt::RightEdge);
    if (!(edges & Qt::BottomEdge))
      return Qt::SizeHorCursor;
    if (!horizontal)
      return Qt::SizeVerCursor;
    return edges & Qt::LeftEdge ? Qt::SizeBDiagCursor : Qt::SizeFDiagCursor;
  }

  void showEdges(Qt::Edges edges) {
    if (edges == shown_)
      return;
    if (shown_)
      QGuiApplication::restoreOverrideCursor();
    if (edges)
      QGuiApplication::setOverrideCursor(shapeFor(edges));
    shown_ = edges;
  }

  QWidget &window_;
  QWindow &handle_;
  Qt::Edges shown_;
};
} // namespace

void widenResizeEdges(QWidget &window) {
  if (!onGnome() || !QGuiApplication::platformName().startsWith(
                        QLatin1String(WaylandPlatform)))
    return;
  if (QWindow *handle = window.windowHandle())
    new ResizeEdges(window, *handle);
}

} // namespace neverd::gui::gnome
