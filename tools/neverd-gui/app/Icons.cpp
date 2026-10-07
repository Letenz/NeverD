#include "Icons.h"

#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPixmapCache>
#include <QSvgRenderer>
#include <memory>

namespace neverd::gui {
namespace {
constexpr char IconPrefix[] = ":/neverd/icons/";
constexpr qreal DisabledOpacity = 0.35;

class SvgIconEngine final : public QIconEngine {
public:
  SvgIconEngine(QString name, std::shared_ptr<QSvgRenderer> renderer)
      : name_(std::move(name)), renderer_(std::move(renderer)) {}

  void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode,
             QIcon::State) override {
    painter->save();
    if (mode == QIcon::Disabled)
      painter->setOpacity(painter->opacity() * DisabledOpacity);
    renderer_->render(painter, QRectF(rect));
    painter->restore();
  }

  QPixmap pixmap(const QSize &size, QIcon::Mode mode,
                 QIcon::State state) override {
    return scaledPixmap(size, mode, state, 1.0);
  }

  QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state,
                       qreal scale) override {
    const QSize device = size * scale;
    const QString key = QStringLiteral("neverd-icon:%1:%2x%3:%4")
                            .arg(name_)
                            .arg(device.width())
                            .arg(device.height())
                            .arg(int(mode));
    QPixmap result;
    if (!QPixmapCache::find(key, &result)) {
      result = QPixmap(device);
      result.fill(Qt::transparent);
      QPainter painter(&result);
      paint(&painter, QRect(QPoint(), device), mode, state);
      painter.end();
      QPixmapCache::insert(key, result);
    }
    result.setDevicePixelRatio(scale);
    return result;
  }

  QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override {
    return size;
  }
  QIconEngine *clone() const override {
    return new SvgIconEngine(name_, renderer_);
  }
  QString key() const override { return QStringLiteral("neverd-svg"); }

private:
  QString name_;
  std::shared_ptr<QSvgRenderer> renderer_;
};
} // namespace

QIcon icon(const QString &name) {
  static QHash<QString, QIcon> cache;
  if (auto it = cache.constFind(name); it != cache.cend())
    return *it;
  QIcon result;
  const QString path = IconPrefix + name + QStringLiteral(".svg");
  if (QFile::exists(path)) {
    auto renderer = std::make_shared<QSvgRenderer>(path);
    if (renderer->isValid())
      result = QIcon(new SvgIconEngine(name, std::move(renderer)));
  }
  cache.insert(name, result);
  return result;
}

} // namespace neverd::gui
