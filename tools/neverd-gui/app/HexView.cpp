#include "HexView.h"

#include "AddressSpace.h"
#include "Session.h"
#include "Theme.h"

#include <QJsonObject>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>
#include <algorithm>

namespace neverd::gui {
namespace {
constexpr int BytesPerRow = 16;
constexpr int ChunkBytes = 4096;
constexpr int MaxChunks = 256;
constexpr int Margin = 6;
// Columns: address, two spaces, 16 hex bytes ("xx " each, extra space after
// eight), two spaces, 16 characters.
constexpr int HexGroup = 8;

Address chunkBase(Address address) {
  return address & ~Address(ChunkBytes - 1);
}
} // namespace

HexView::HexView(Session &session, const AddressSpace &space, QWidget *parent)
    : QAbstractScrollArea(parent), session_(session), space_(space) {
  setFocusPolicy(Qt::StrongFocus);
  setFrameShape(QFrame::NoFrame);
  viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
  updateMetrics();
  connect(&Theme::instance(), &Theme::changed, this, [this] {
    updateMetrics();
    viewport()->update();
  });
  const auto reset = [this] {
    ++serial_;
    chunks_.clear();
    order_.clear();
    pending_.clear();
    updateRange();
    viewport()->update();
  };
  connect(&session_, &Session::opened, this, reset);
  connect(&session_, &Session::unloaded, this, [this, reset] {
    current_.reset();
    reset();
  });
}

void HexView::updateMetrics() {
  const QFontMetricsF metrics(Theme::instance().codeFont());
  charWidth_ = metrics.horizontalAdvance(QLatin1Char('0'));
  lineHeight_ = int(std::ceil(metrics.lineSpacing()));
  ascent_ = int(std::ceil(metrics.ascent()));
  updateRange();
}

int HexView::visibleRows() const {
  return std::max(1, viewport()->height() / std::max(1, lineHeight_));
}

qint64 HexView::totalRows() const {
  qint64 rows = 0;
  for (const auto &region : space_.regions())
    rows += qint64((region.end - region.start + BytesPerRow - 1) / BytesPerRow);
  return rows;
}

qint64 HexView::rowOf(Address address) const {
  qint64 rows = 0;
  for (const auto &region : space_.regions()) {
    if (address >= region.start && address < region.end)
      return rows + qint64((address - region.start) / BytesPerRow);
    rows += qint64((region.end - region.start + BytesPerRow - 1) / BytesPerRow);
  }
  return 0;
}

std::optional<Address> HexView::addressOfRow(qint64 row) const {
  for (const auto &region : space_.regions()) {
    const qint64 count =
        qint64((region.end - region.start + BytesPerRow - 1) / BytesPerRow);
    if (row < count)
      return region.start + Address(row) * BytesPerRow;
    row -= count;
  }
  return std::nullopt;
}

void HexView::updateRange() {
  const qint64 rows = totalRows();
  auto *bar = verticalScrollBar();
  bar->setRange(0,
                int(std::min<qint64>(std::max<qint64>(0, rows - visibleRows()),
                                     std::numeric_limits<int>::max())));
  bar->setPageStep(visibleRows());
  bar->setSingleStep(1);
}

void HexView::resizeEvent(QResizeEvent *event) {
  QAbstractScrollArea::resizeEvent(event);
  updateRange();
}

void HexView::scrollContentsBy(int, int) { viewport()->update(); }

const HexView::Chunk *HexView::chunk(Address base) const {
  if (auto it = chunks_.constFind(base); it != chunks_.cend())
    return &*it;
  request(base);
  return nullptr;
}

void HexView::request(Address base) const {
  if (pending_.contains(base) || !session_.loaded())
    return;
  pending_.insert(base);
  const quint64 serial = serial_;
  auto *self = const_cast<HexView *>(this);
  session_.read(
      QStringLiteral("bytes"),
      {{"address", hexAddress(base)}, {"size", ChunkBytes}}, self,
      [self, base, serial](const QJsonObject &payload) {
        if (serial != self->serial_)
          return;
        self->pending_.remove(base);
        Chunk chunk;
        chunk.data =
            QByteArray::fromHex(payload.value("data").toString().toLatin1());
        chunk.mapped = int(chunk.data.size());
        self->chunks_.insert(base, chunk);
        self->order_.push_front(base);
        while (int(self->order_.size()) > MaxChunks) {
          self->chunks_.remove(self->order_.back());
          self->order_.pop_back();
        }
        self->viewport()->update();
      },
      [self, base](const QString &, const QString &) {
        self->pending_.remove(base);
      });
}

void HexView::addressSpaceChanged() {
  updateRange();
  if (current_)
    setCurrent(*current_, currentSize_);
  else
    viewport()->update();
}

std::optional<quint8> HexView::byteAt(Address address) const {
  const auto it = chunks_.constFind(chunkBase(address));
  if (it == chunks_.cend())
    return std::nullopt;
  const int offset = int(address - it.key());
  if (offset >= it->mapped)
    return std::nullopt;
  return static_cast<quint8>(it->data.at(offset));
}

void HexView::setCurrent(Address address, int size) {
  current_ = address;
  currentSize_ = std::max(1, size);
  const qint64 row = rowOf(address);
  auto *bar = verticalScrollBar();
  if (row < bar->value() || row >= bar->value() + visibleRows())
    bar->setValue(int(std::max<qint64>(0, row - visibleRows() / 3)));
  viewport()->update();
}

void HexView::paintEvent(QPaintEvent *) {
  QPainter painter(viewport());
  const auto &theme = Theme::instance();
  painter.fillRect(viewport()->rect(), theme.color(ColorRole::HexBackground));
  if (space_.empty())
    return;
  painter.setFont(theme.codeFont());
  const int digits = session_.bitness() == 64 ? 16 : 8;
  const qreal hexLeft = Margin + (digits + 2) * charWidth_;
  const qreal asciiLeft = hexLeft + (BytesPerRow * 3 + 2) * charWidth_;
  const qint64 first = verticalScrollBar()->value();
  for (int i = 0; i <= visibleRows(); ++i) {
    const auto rowAddress = addressOfRow(first + i);
    if (!rowAddress)
      break;
    const int y = i * lineHeight_;
    const auto *region = space_.regionOf(*rowAddress);
    painter.setPen(theme.color(ColorRole::HexAddress));
    painter.drawText(QPointF(Margin, y + ascent_),
                     displayAddress(*rowAddress, digits));
    const Address base = chunkBase(*rowAddress);
    const Chunk *data = chunk(base);
    // A row may straddle two chunks only when regions are unaligned.
    const Chunk *next = nullptr;
    if (chunkBase(*rowAddress + BytesPerRow - 1) != base)
      next = chunk(chunkBase(*rowAddress + BytesPerRow - 1));
    for (int b = 0; b < BytesPerRow; ++b) {
      const Address address = *rowAddress + b;
      if (region && address >= region->end)
        break;
      const qreal x = hexLeft + (b * 3 + (b >= HexGroup ? 1 : 0)) * charWidth_;
      const qreal ax = asciiLeft + b * charWidth_;
      const Chunk *source = chunkBase(address) == base ? data : next;
      const int offset = int(address - chunkBase(address));
      const bool mapped = source && offset < source->mapped;
      const bool current = current_ && address >= *current_ &&
                           address < *current_ + Address(currentSize_);
      if (current) {
        painter.fillRect(
            QRectF(x - charWidth_ / 4, y, charWidth_ * 2.5, lineHeight_),
            theme.color(ColorRole::HexCurrent));
        painter.fillRect(QRectF(ax, y, charWidth_, lineHeight_),
                         theme.color(ColorRole::HexCurrent));
      }
      if (!source) {
        painter.setPen(theme.color(ColorRole::HexUnmapped));
        painter.drawText(QPointF(x, y + ascent_), QStringLiteral(".."));
        continue;
      }
      if (!mapped) {
        painter.setPen(theme.color(ColorRole::HexUnmapped));
        painter.drawText(QPointF(x, y + ascent_), QStringLiteral("??"));
        painter.drawText(QPointF(ax, y + ascent_), QStringLiteral(" "));
        continue;
      }
      const auto value = static_cast<unsigned char>(source->data.at(offset));
      painter.setPen(current ? theme.color(ColorRole::HexCurrentText)
                     : value ? theme.color(ColorRole::HexByte)
                             : theme.color(ColorRole::HexZero));
      painter.drawText(
          QPointF(x, y + ascent_),
          QStringLiteral("%1").arg(value, 2, 16, QLatin1Char('0')).toUpper());
      painter.setPen(current ? theme.color(ColorRole::HexCurrentText)
                             : theme.color(ColorRole::HexAscii));
      const QChar character =
          value >= 0x20 && value < 0x7f ? QChar(value) : QLatin1Char('.');
      painter.drawText(QPointF(ax, y + ascent_), QString(character));
    }
  }
}

void HexView::keyPressEvent(QKeyEvent *event) {
  if (!current_) {
    QAbstractScrollArea::keyPressEvent(event);
    return;
  }
  Address next = *current_;
  switch (event->key()) {
  case Qt::Key_Left:
    next = next ? next - 1 : 0;
    break;
  case Qt::Key_Right:
    next += 1;
    break;
  case Qt::Key_Up:
    next = next >= BytesPerRow ? next - BytesPerRow : next;
    break;
  case Qt::Key_Down:
    next += BytesPerRow;
    break;
  case Qt::Key_PageUp:
    next = next >= Address(BytesPerRow * visibleRows())
               ? next - BytesPerRow * visibleRows()
               : 0;
    break;
  case Qt::Key_PageDown:
    next += Address(BytesPerRow * visibleRows());
    break;
  default:
    QAbstractScrollArea::keyPressEvent(event);
    return;
  }
  if (!space_.regionOf(next))
    return;
  setCurrent(next, 1);
  emit locationChanged(next);
}

void HexView::mousePressEvent(QMouseEvent *event) {
  setFocus(Qt::MouseFocusReason);
  const int digits = session_.bitness() == 64 ? 16 : 8;
  const qreal hexLeft = Margin + (digits + 2) * charWidth_;
  const qreal asciiLeft = hexLeft + (BytesPerRow * 3 + 2) * charWidth_;
  const int rowIndex = int(event->position().y()) / std::max(1, lineHeight_);
  const auto rowAddress = addressOfRow(verticalScrollBar()->value() + rowIndex);
  if (!rowAddress)
    return;
  const qreal x = event->position().x();
  int byte = -1;
  if (x >= asciiLeft)
    byte = int((x - asciiLeft) / charWidth_);
  else if (x >= hexLeft) {
    qreal column = (x - hexLeft) / charWidth_;
    if (column >= HexGroup * 3)
      column -= 1;
    byte = int(column / 3);
  }
  if (byte < 0 || byte >= BytesPerRow)
    return;
  const Address address = *rowAddress + Address(byte);
  if (!space_.regionOf(address))
    return;
  setCurrent(address, 1);
  emit locationChanged(address);
}

void HexView::wheelEvent(QWheelEvent *event) {
  if (event->modifiers() & Qt::ControlModifier) {
    Theme::instance().zoomCodeFont(event->angleDelta().y() > 0 ? 1 : -1);
    return;
  }
  QAbstractScrollArea::wheelEvent(event);
}

} // namespace neverd::gui
