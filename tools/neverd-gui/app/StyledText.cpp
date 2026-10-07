#include "StyledText.h"

#include <QFontMetricsF>
#include <QTextCharFormat>

namespace neverd::gui {
namespace {
/// Whether a fixed-width listing draws \p code two columns wide; the worker
/// aligns columns by the same table.
bool isWideCharacter(char32_t code) {
#define NEVERD_WIDE_CHARACTERS(First, Last)                                    \
  if (code >= (First) && code <= (Last))                                       \
    return true;
#include "neverd/support/WideCharacters.def"
  return false;
}
} // namespace

bool isTokenCharacter(QChar c) {
  return c.isLetterOrNumber() || c == QLatin1Char('_') ||
         c == QLatin1Char('$') || c == QLatin1Char('@') ||
         c == QLatin1Char('?') || c == QLatin1Char('.');
}

void StyledLine::setFromWorker(const QString &line,
                               const QJsonArray &workerSpans) {
  text = line;
  spans.clear();
  spans.reserve(workerSpans.size());
  layout_.reset();
  // Worker offsets count UTF-8 bytes.  For ASCII both encodings agree; other
  // text maps each byte offset to its UTF-16 position.
  bool ascii = true;
  for (const QChar c : line)
    if (c.unicode() >= 0x80) {
      ascii = false;
      break;
    }
  QVector<int> byteToUtf16;
  if (!ascii) {
    const QByteArray utf8 = line.toUtf8();
    byteToUtf16.resize(utf8.size() + 1);
    int utf16 = 0;
    for (int byte = 0; byte < utf8.size();) {
      const auto lead = static_cast<unsigned char>(utf8[byte]);
      const int width = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
      for (int k = 0; k < width && byte + k < utf8.size(); ++k)
        byteToUtf16[byte + k] = utf16;
      byte += width;
      utf16 += width == 4 ? 2 : 1;
    }
    byteToUtf16[utf8.size()] = utf16;
  }
  const auto position = [&](int byte) {
    if (ascii)
      return std::clamp(byte, 0, int(line.size()));
    return byteToUtf16.value(std::clamp(byte, 0, int(byteToUtf16.size()) - 1));
  };
  for (const auto &value : workerSpans) {
    const auto entry = value.toArray();
    if (entry.size() < 3)
      continue;
    const int begin = position(entry.at(0).toInt());
    const int end = position(entry.at(0).toInt() + entry.at(1).toInt());
    StyledSpan span{begin, end - begin, entry.at(2).toInt(), std::nullopt};
    if (entry.size() > 3)
      span.address = addressValue(entry.at(3));
    if (span.length > 0)
      spans.append(span);
  }
}

const StyledSpan *StyledLine::spanAt(int position) const {
  for (const auto &span : spans)
    if (position >= span.start && position < span.start + span.length)
      return &span;
  return nullptr;
}

QString StyledLine::tokenAt(int position) const {
  if (position < 0 || position > text.size())
    return {};
  int begin = position, end = position;
  while (begin > 0 && isTokenCharacter(text.at(begin - 1)))
    --begin;
  while (end < text.size() && isTokenCharacter(text.at(end)))
    ++end;
  return text.mid(begin, end - begin);
}

QTextLayout &StyledLine::layout(const QFont &font, quint64 stamp,
                                const ColorOf &colorOf) const {
  if (layout_ && stamp_ == stamp)
    return *layout_;
  layout_ = std::make_unique<QTextLayout>(text, font);
  layout_->setCacheEnabled(true);
  QList<QTextLayout::FormatRange> formats;
  formats.reserve(spans.size() + 1);
  QTextLayout::FormatRange base;
  base.start = 0;
  base.length = text.size();
  base.format.setForeground(colorOf(0));
  formats.append(base);
  for (const auto &span : spans) {
    QTextLayout::FormatRange range;
    range.start = span.start;
    range.length = span.length;
    range.format.setForeground(colorOf(span.role));
    formats.append(range);
  }
  // A wide character's glyph comes from whichever font has it; spacing
  // makes it exactly two columns, so the columns after it line up.
  const QFontMetricsF metrics(font);
  const qreal column = metrics.horizontalAdvance(QLatin1Char('M'));
  for (qsizetype i = 0; i < text.size();) {
    const bool pair = text.at(i).isHighSurrogate() && i + 1 < text.size() &&
                      text.at(i + 1).isLowSurrogate();
    const char32_t code =
        pair ? QChar::surrogateToUcs4(text.at(i), text.at(i + 1))
             : text.at(i).unicode();
    const int units = pair ? 2 : 1;
    if (isWideCharacter(code)) {
      QTextLayout::FormatRange range;
      range.start = int(i);
      range.length = units;
      range.format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
      range.format.setFontLetterSpacing(
          2 * column - metrics.horizontalAdvance(text.mid(i, units)));
      formats.append(range);
    }
    i += units;
  }
  layout_->setFormats(formats);
  QTextOption option;
  option.setWrapMode(QTextOption::NoWrap);
  layout_->setTextOption(option);
  layout_->beginLayout();
  QTextLine line = layout_->createLine();
  if (line.isValid())
    line.setPosition(QPointF(0, 0));
  layout_->endLayout();
  stamp_ = stamp;
  return *layout_;
}

QVector<int> tokenOccurrences(const QString &text, const QString &token) {
  QVector<int> result;
  if (token.isEmpty())
    return result;
  for (qsizetype at = text.indexOf(token); at >= 0;
       at = text.indexOf(token, at + 1)) {
    const bool startOk = at == 0 || !isTokenCharacter(text.at(at - 1));
    const qsizetype after = at + token.size();
    const bool endOk =
        after >= text.size() || !isTokenCharacter(text.at(after));
    if (startOk && endOk)
      result.append(int(at));
  }
  return result;
}

} // namespace neverd::gui
