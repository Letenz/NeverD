#include "HexView.h"

#include "AddressSpace.h"
#include "Session.h"
#include "Theme.h"

#include <QActionGroup>
#include <QContextMenuEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QSettings>
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
constexpr char TextEncodingKey[] = "hex/textEncoding";
/// What the text column shows for a byte that starts no shown character.
constexpr char HiddenByte = '.';

Address chunkBase(Address address) {
  return address & ~Address(ChunkBytes - 1);
}
} // namespace

HexView::HexView(Session &session, const AddressSpace &space, QWidget *parent)
    : QAbstractScrollArea(parent), session_(session), space_(space) {
  setFocusPolicy(Qt::StrongFocus);
  setFrameShape(QFrame::NoFrame);
  viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
  textEncoding_ = QSettings().value(TextEncodingKey).toString();
  updateMetrics();
  connect(&Theme::instance(), &Theme::changed, this, [this] {
    updateMetrics();
    viewport()->update();
  });
  connect(&session_, &Session::opened, this, [this] {
    clearChunks();
    loadEncodings();
  });
  connect(&session_, &Session::unloaded, this, [this] {
    current_.reset();
    clearChunks();
  });
}

void HexView::clearChunks() {
  ++serial_;
  chunks_.clear();
  order_.clear();
  pending_.clear();
  updateRange();
  viewport()->update();
}

void HexView::loadEncodings() {
  session_.read(
      QStringLiteral("string_encodings"), {}, this,
      [this](const QJsonObject &payload) {
        encodings_.clear();
        for (const auto &value : payload.value("items").toArray()) {
          const auto encoding = value.toObject();
          const auto spelling = encoding.value("spelling").toString();
          encodings_.append(
              {encoding.value("name").toString(),
               spelling.isEmpty() ? QStringLiteral("ASCII") : spelling,
               encoding.value("legacy").toBool()});
        }
      },
      [](const QString &, const QString &) {});
}

void HexView::setTextEncoding(const QString &name) {
  if (name == textEncoding_)
    return;
  textEncoding_ = name;
  QSettings settings;
  if (name.isEmpty())
    settings.remove(TextEncodingKey);
  else
    settings.setValue(TextEncodingKey, name);
  clearChunks();
}

void HexView::contextMenuEvent(QContextMenuEvent *event) {
  QMenu menu(this);
  auto *encodings = menu.addMenu(tr("Text encoding"));
  auto *group = new QActionGroup(encodings);
  const auto add = [&](const QString &label, const QString &name) {
    auto *action = encodings->addAction(label);
    action->setCheckable(true);
    action->setChecked(textEncoding_ == name);
    group->addAction(action);
    connect(action, &QAction::triggered, this,
            [this, name] { setTextEncoding(name); });
  };
  // Plain ASCII needs no engine; Unicode encodings come before code pages.
  add(QStringLiteral("ASCII"), {});
  for (const auto &encoding : encodings_)
    if (!encoding.legacy && encoding.label != QLatin1String("ASCII"))
      add(encoding.label, encoding.name);
  encodings->addSeparator();
  for (const auto &encoding : encodings_)
    if (encoding.legacy)
      add(encoding.label, encoding.name);
  menu.exec(event->globalPos());
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

void HexView::buildRows() {
  rows_.clear();
  rowCount_ = 0;
  for (const auto &region : space_.regions()) {
    if (region.end <= region.start)
      continue;
    const Address start = region.start & ~Address(BytesPerRow - 1);
    const Address last = (region.end - 1) & ~Address(BytesPerRow - 1);
    // Regions sharing or abutting a row extend the run they meet.
    if (!rows_.isEmpty() && start <= rows_.back().end) {
      rows_.back().end = std::max(rows_.back().end, last + BytesPerRow);
      continue;
    }
    rows_.append({start, last + BytesPerRow, 0});
  }
  for (auto &run : rows_) {
    run.first = rowCount_;
    rowCount_ += qint64((run.end - run.start) / BytesPerRow);
  }
}

qint64 HexView::rowOf(Address address) const {
  for (const auto &run : rows_)
    if (address >= run.start && address < run.end)
      return run.first + qint64((address - run.start) / BytesPerRow);
  return 0;
}

std::optional<Address> HexView::addressOfRow(qint64 row) const {
  const auto run = std::upper_bound(
      rows_.begin(), rows_.end(), row,
      [](qint64 value, const RowRun &r) { return value < r.first; });
  if (run == rows_.begin())
    return std::nullopt;
  const auto &found = *std::prev(run);
  const Address address =
      found.start + Address(row - found.first) * BytesPerRow;
  return address < found.end ? std::optional(address) : std::nullopt;
}

void HexView::updateRange() {
  buildRows();
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

Address HexView::chunkKey(Address address) const {
  const Address base = chunkBase(address);
  const auto *region = space_.regionOf(address);
  return region && region->start > base ? region->start : base;
}

const HexView::Chunk *HexView::chunk(Address key) const {
  if (auto it = chunks_.constFind(key); it != chunks_.cend())
    return &*it;
  request(key);
  return nullptr;
}

void HexView::request(Address key) const {
  if (pending_.contains(key) || !session_.loaded())
    return;
  // Reading stops at the first unmapped byte, so a chunk ends with its
  // region: bytes past a gap belong to the next region's chunk.
  Address end = chunkBase(key) + ChunkBytes;
  if (const auto *region = space_.regionOf(key))
    end = std::min(end, region->end);
  const Address base = key;
  pending_.insert(base);
  const quint64 serial = serial_;
  auto *self = const_cast<HexView *>(this);
  QJsonObject request{{"address", hexAddress(base)},
                      {"size", qint64(end - base)}};
  // A character split across two chunks decodes as two hidden halves.
  if (!textEncoding_.isEmpty())
    request.insert("text_encoding", textEncoding_);
  session_.read(
      QStringLiteral("bytes"), request, self,
      [self, base, serial](const QJsonObject &payload) {
        if (serial != self->serial_)
          return;
        self->pending_.remove(base);
        Chunk chunk;
        chunk.data =
            QByteArray::fromHex(payload.value("data").toString().toLatin1());
        chunk.mapped = int(chunk.data.size());
        for (const auto &cell : payload.value("cells").toArray())
          chunk.cells.append(cell.isNull() ? QString(QLatin1Char(HiddenByte))
                                           : cell.toString());
        self->chunks_.insert(base, chunk);
        self->order_.push_front(base);
        while (int(self->order_.size()) > MaxChunks) {
          self->chunks_.remove(self->order_.back());
          self->order_.pop_back();
        }
        self->viewport()->update();
      },
      [self, base, serial](const QString &code, const QString &) {
        if (serial != self->serial_)
          return;
        self->pending_.remove(base);
        // An encoding this engine does not know, or an engine that decodes
        // no text, reads as ASCII again.
        if (code == QLatin1String("unsupported_encoding") ||
            code == QLatin1String("unsupported"))
          self->setTextEncoding({});
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
  const auto it = chunks_.constFind(chunkKey(address));
  if (it == chunks_.cend())
    return std::nullopt;
  const int offset = int(address - it.key());
  if (offset >= it->mapped)
    return std::nullopt;
  return static_cast<quint8>(it->data.at(offset));
}

std::optional<QString> HexView::textAt(Address address) const {
  const auto it = chunks_.constFind(chunkKey(address));
  if (it == chunks_.cend())
    return std::nullopt;
  const int offset = int(address - it.key());
  if (offset >= it->mapped)
    return std::nullopt;
  if (!it->cells.isEmpty())
    return it->cells.at(offset);
  const auto value = static_cast<unsigned char>(it->data.at(offset));
  return QString(value >= 0x20 && value < 0x7f ? QChar(value)
                                               : QLatin1Char(HiddenByte));
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
    painter.setPen(theme.color(ColorRole::HexAddress));
    painter.drawText(QPointF(Margin, y + ascent_),
                     displayAddress(*rowAddress, digits));
    const auto isCurrent = [&](Address address) {
      return current_ && address >= *current_ &&
             address < *current_ + Address(currentSize_);
    };
    // Highlights go first: a wide character's glyph spans the next cells.
    for (int b = 0; b < BytesPerRow; ++b) {
      const Address address = *rowAddress + b;
      if (!isCurrent(address) || !space_.regionOf(address))
        continue;
      const qreal x = hexLeft + (b * 3 + (b >= HexGroup ? 1 : 0)) * charWidth_;
      painter.fillRect(
          QRectF(x - charWidth_ / 4, y, charWidth_ * 2.5, lineHeight_),
          theme.color(ColorRole::HexCurrent));
      painter.fillRect(
          QRectF(asciiLeft + b * charWidth_, y, charWidth_, lineHeight_),
          theme.color(ColorRole::HexCurrent));
    }
    for (int b = 0; b < BytesPerRow; ++b) {
      const Address address = *rowAddress + b;
      // A byte no region maps stays blank, as between two sections.
      if (!space_.regionOf(address))
        continue;
      const qreal x = hexLeft + (b * 3 + (b >= HexGroup ? 1 : 0)) * charWidth_;
      const qreal ax = asciiLeft + b * charWidth_;
      const Address key = chunkKey(address);
      const Chunk *source = chunk(key);
      const int offset = int(address - key);
      const bool mapped = source && offset < source->mapped;
      const bool current = isCurrent(address);
      if (!source) {
        painter.setPen(theme.color(ColorRole::HexUnmapped));
        painter.drawText(QPointF(x, y + ascent_), QStringLiteral(".."));
        continue;
      }
      if (!mapped) {
        painter.setPen(theme.color(ColorRole::HexUnmapped));
        painter.drawText(QPointF(x, y + ascent_), QStringLiteral("??"));
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
      if (!source->cells.isEmpty()) {
        // A character's continuation bytes show nothing of their own.
        if (const auto &cell = source->cells.at(offset); !cell.isEmpty())
          painter.drawText(QPointF(ax, y + ascent_), cell);
        continue;
      }
      const QChar character = value >= 0x20 && value < 0x7f
                                  ? QChar(value)
                                  : QLatin1Char(HiddenByte);
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
