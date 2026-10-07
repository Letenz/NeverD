#include "LibraryCodeView.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString summaryText(QString text) {
  for (auto &c : text)
    if (!c.isPrint())
      c = QChar(' ');
  return text.simplified().replace("*/", "* /").left(512);
}
} // namespace

void LibraryCodeView::reset(const QString &original,
                            const QVariantList &mappings,
                            const QJsonArray &regions, qint64 byteOffset,
                            bool keepFolded) {
  if (!keepFolded)
    folded_.clear();
  original_ = original;
  originalMappings_ = mappings;
  regions_.clear();
  const QByteArray bytes = original.toUtf8();
  auto position = [&](const QJsonValue &value) -> int {
    // Reject absent, fractional, negative and overflowing offsets. No rounding
    // a byte in the middle of a multibyte character onto a Qt position.
    const auto number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || byteOffset < 0 ||
        byteOffset > 32 * 1024 * 1024 || number < byteOffset ||
        number - byteOffset > bytes.size() ||
        number != static_cast<double>(static_cast<qint64>(number)))
      return -1;
    const auto local = static_cast<qsizetype>(number - byteOffset);
    if (local < bytes.size() &&
        (static_cast<unsigned char>(bytes[local]) & 0xc0) == 0x80)
      return -1;
    return QString::fromUtf8(bytes.constData(), local).size();
  };
  QSet<QString> ids;
  qsizetype spanCount = 0;
  if (regions.size() <= 256)
    for (const auto &value : regions) {
      const auto object = value.toObject();
      Region region;
      region.details = object.toVariantMap();
      region.id = object["id"].toString();
      region.library = !object.contains("kind");
      region.summary = summaryText(object["display_name"].toString());
      region.available = !region.id.isEmpty() && !region.summary.isEmpty() &&
                         object["foldable"].toBool() &&
                         object["mapping_status"] == "mapped";
      const auto spans = object["spans"].toArray();
      spanCount += spans.size();
      region.available &=
          !spans.isEmpty() && spans.size() <= 1024 && spanCount <= 4096;
      if (region.available)
        for (const auto &entry : spans) {
          const auto span = entry.toObject();
          const int begin = position(span["begin_byte"]);
          const int end = position(span["end_byte"]);
          if (begin < 0 || end <= begin) {
            region.available = false;
            break;
          }
          region.spans.push_back({begin, end});
        }
      std::sort(region.spans.begin(), region.spans.end(),
                [](const auto &a, const auto &b) { return a.begin < b.begin; });
      for (qsizetype i = 1; i < region.spans.size(); ++i)
        if (region.spans[i].begin < region.spans[i - 1].end)
          region.available = false;
      if (ids.contains(region.id)) {
        region.available = false;
        for (auto &previous : regions_)
          if (previous.id == region.id)
            previous.available = false;
      }
      ids.insert(region.id);
      regions_.push_back(std::move(region));
    }
  // A partial page and an ambiguous range remain readable in full. Never
  // choose the first overlapping region based on its wire order.
  struct Interval {
    Span span;
    qsizetype region;
  };
  QVector<Interval> intervals;
  for (qsizetype i = 0; i < regions_.size(); ++i)
    if (regions_[i].available)
      for (const auto &span : regions_[i].spans)
        intervals.push_back({span, i});
  std::sort(intervals.begin(), intervals.end(),
            [](auto a, auto b) { return a.span.begin < b.span.begin; });
  int furthest = -1;
  qsizetype owner = 0;
  for (const auto &interval : intervals) {
    if (interval.span.begin < furthest) {
      regions_[owner].available = false;
      regions_[interval.region].available = false;
    }
    if (interval.span.end > furthest) {
      furthest = interval.span.end;
      owner = interval.region;
    }
  }
  QSet<QString> valid;
  for (const auto &region : regions_)
    if (region.available)
      valid.insert(region.id);
  folded_.intersect(valid);
  rebuild();
}

QVariantList LibraryCodeView::regions() const {
  QVariantList result;
  for (const auto &region : regions_) {
    auto details = region.details;
    details["available"] = region.available;
    details["folded"] = folded_.contains(region.id);
    result.push_back(details);
  }
  return result;
}

int LibraryCodeView::foldableCount() const {
  return std::count_if(
      regions_.begin(), regions_.end(),
      [](const auto &region) { return region.available && region.library; });
}

bool LibraryCodeView::libraryFolded() const {
  return std::any_of(regions_.begin(), regions_.end(), [&](const auto &region) {
    return region.library && folded_.contains(region.id);
  });
}

bool LibraryCodeView::canFold() const {
  return std::any_of(regions_.begin(), regions_.end(),
                     [](const auto &region) { return region.available; });
}

bool LibraryCodeView::hasRegion(const QString &id) const {
  return std::any_of(regions_.begin(), regions_.end(), [&](const auto &region) {
    return region.available && region.id == id;
  });
}

int LibraryCodeView::displayLine(int sourceLine) const {
  qsizetype start = 0;
  for (int line = 0; line < sourceLine; ++line) {
    start = original_.indexOf(QLatin1Char('\n'), start);
    if (start < 0)
      return -1;
    ++start;
  }
  return int(QStringView(text_)
                 .left(displayPosition(int(start)))
                 .count(QLatin1Char('\n')));
}

void LibraryCodeView::toggleRegion(const QString &id) {
  setRegionFolded(id, !folded_.contains(id));
}

void LibraryCodeView::setRegionFolded(const QString &id, bool folded) {
  for (const auto &region : regions_)
    if (region.id == id && region.available) {
      if (folded)
        folded_.insert(id);
      else
        folded_.remove(id);
      rebuild();
      return;
    }
}

void LibraryCodeView::setFolded(bool folded) {
  for (const auto &region : regions_)
    if (region.available && region.library) {
      if (folded)
        folded_.insert(region.id);
      else
        folded_.remove(region.id);
    }
  rebuild();
}

void LibraryCodeView::rebuild() {
  struct Fold {
    Span span;
    QString id, summary;
  };
  QVector<Fold> folds;
  for (const auto &region : regions_)
    if (region.available && folded_.contains(region.id))
      for (const auto &span : region.spans)
        folds.push_back({span, region.id, region.summary});
  std::sort(folds.begin(), folds.end(), [](const auto &a, const auto &b) {
    return a.span.begin < b.span.begin;
  });
  text_.clear();
  segments_.clear();
  int cursor = 0;
  auto append = [&](int begin, int end, const QString &text,
                    const QString &id) {
    if (text.isEmpty())
      return;
    const int start = text_.size();
    text_ += text;
    segments_.push_back(
        {start, static_cast<int>(text_.size()), begin, end, id});
  };
  for (const auto &fold : folds) {
    append(cursor, fold.span.begin,
           original_.mid(cursor, fold.span.begin - cursor), {});
    append(fold.span.begin, fold.span.end, "/* " + fold.summary + " … */",
           fold.id);
    cursor = fold.span.end;
  }
  append(cursor, original_.size(), original_.mid(cursor), {});

  QVector<int> sourceLines{0}, displayLines{0};
  for (int i = 0; i < original_.size(); ++i)
    if (original_[i] == '\n')
      sourceLines.push_back(i + 1);
  for (int i = 0; i < text_.size(); ++i)
    if (text_[i] == '\n')
      displayLines.push_back(i + 1);
  mappings_.clear();
  for (const auto &entry : originalMappings_) {
    auto row = entry.toMap();
    const int line = row["line"].toInt();
    if (line < 0 || line >= sourceLines.size())
      continue;
    const int at = displayPosition(sourceLines[line]);
    row["line"] =
        std::upper_bound(displayLines.begin(), displayLines.end(), at) -
        displayLines.begin() - 1;
    mappings_.push_back(row);
  }
  emit changed();
}

int LibraryCodeView::displayPosition(int position) const {
  for (const auto &segment : segments_)
    if (position >= segment.sourceBegin && position < segment.sourceEnd)
      return segment.region.isEmpty()
                 ? segment.begin + position - segment.sourceBegin
                 : segment.begin;
  return text_.size();
}

int LibraryCodeView::sourcePosition(int position, bool end) const {
  position = std::clamp(position, 0, static_cast<int>(text_.size()));
  for (const auto &segment : segments_) {
    if (position == segment.begin)
      return segment.sourceBegin;
    if (position > segment.begin && position < segment.end)
      return segment.region.isEmpty()
                 ? segment.sourceBegin + position - segment.begin
                 : (end ? segment.sourceEnd : segment.sourceBegin);
  }
  return original_.size();
}

QVector<QPair<int, int>> LibraryCodeView::foldedRanges() const {
  QVector<QPair<int, int>> ranges;
  for (const auto &segment : segments_)
    if (!segment.region.isEmpty())
      ranges.append({segment.begin, segment.end});
  return ranges;
}

QString LibraryCodeView::regionAt(int position) const {
  for (const auto &segment : segments_)
    if (position >= segment.begin && position < segment.end)
      return segment.region;
  return {};
}

int LibraryCodeView::sourceLineAt(int position) const {
  return QStringView(original_)
      .left(sourcePosition(position, false))
      .count('\n');
}

QString LibraryCodeView::originalSelection(int begin, int end) const {
  if (begin >= end)
    return {};
  const int start = sourcePosition(begin, false);
  return original_.mid(start, sourcePosition(end, true) - start);
}

void LibraryCodeView::copySelection(int begin, int end) const {
  if (auto *clipboard = QGuiApplication::clipboard())
    clipboard->setText(originalSelection(begin, end));
}
