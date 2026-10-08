#include "CodeView.h"

#include "Icons.h"
#include "Session.h"
#include "Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QTextLine>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace neverd::gui {
namespace {
constexpr int PageLines = 2048;
constexpr int Margin = 6;
constexpr int WheelLines = 3;
/// The fold over the lines before a C definition.
constexpr char PreludeRegion[] = "prelude";

/// How a representation is colored; Source takes the language the page names.
enum class Dialect { C, Rust, Go, IR, LLVM, Source };
struct Representation {
  const char *name;
  const char *title;
  Dialect dialect;
};
constexpr Representation Representations[] = {
#define NEVERD_REPRESENTATION(Name, Title, Kind) {Name, Title, Dialect::Kind},
#include "Representations.def"
};

const Representation *representationOf(const QString &name) {
  for (const auto &entry : Representations)
    if (name == QLatin1String(entry.name))
      return &entry;
  return nullptr;
}

// Code token roles, colored through the Code* theme colors.
enum CodeRole : int {
  Default,
  Keyword,
  Control,
  Type,
  Number,
  String,
  Comment,
  Function,
  Variable,
  Constant,
  Punctuation,
  Preprocessor
};

QColor codeColor(int role) {
  static constexpr ColorRole Map[] = {
      ColorRole::CodeDefault,     ColorRole::CodeKeyword,
      ColorRole::CodeControl,     ColorRole::CodeType,
      ColorRole::CodeNumber,      ColorRole::CodeString,
      ColorRole::CodeComment,     ColorRole::CodeFunction,
      ColorRole::CodeVariable,    ColorRole::CodeConstant,
      ColorRole::CodePunctuation, ColorRole::CodePreprocessor};
  return Theme::instance().color(
      Map[role >= 0 && role < int(std::size(Map)) ? role : 0]);
}

/// A source language's words: control flow, other keywords and types.
struct Vocabulary {
  QSet<QString> control, keywords, types;
};

const Vocabulary &vocabularyOf(Dialect dialect) {
  static const Vocabulary C = {{
#define NEVERD_C_CONTROL(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
                               },
                               {
#define NEVERD_C_KEYWORD(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
                               },
                               {
#define NEVERD_C_TYPE(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
                               }};
  static const Vocabulary Rust = {{
#define NEVERD_RUST_CONTROL(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
                                  },
                                  {
#define NEVERD_RUST_KEYWORD(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
                                  },
                                  {
#define NEVERD_RUST_TYPE(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
                                  }};
  static const Vocabulary Go = {{
#define NEVERD_GO_CONTROL(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
                                },
                                {
#define NEVERD_GO_KEYWORD(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
                                },
                                {
#define NEVERD_GO_TYPE(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
                                }};
  return dialect == Dialect::Rust ? Rust : dialect == Dialect::Go ? Go : C;
}

/// Whether \p word names one of the integer or floating types the dialect
/// spells by width: Rust's `u64` and `i96`, Go's `uint64`.
bool widthType(Dialect dialect, const QString &word) {
  static const QRegularExpression Rust(QStringLiteral("^[iuf][0-9]+$"));
  static const QRegularExpression Go(QStringLiteral("^(u?int|float)[0-9]+$"));
  return (dialect == Dialect::Rust && Rust.match(word).hasMatch()) ||
         (dialect == Dialect::Go && Go.match(word).hasMatch());
}

/// The dialect a page reads in: its representation's, or for pseudocode in
/// the function's own language the one the page names.
Dialect dialectOf(const QString &representation, const QString &language) {
  const auto *entry = representationOf(representation);
  const Dialect dialect = entry ? entry->dialect : Dialect::C;
  if (dialect != Dialect::Source)
    return dialect;
  if (language == QLatin1String("rust"))
    return Dialect::Rust;
  if (language == QLatin1String("go"))
    return Dialect::Go;
  return Dialect::C;
}

/// Source languages share C's comments, strings and preprocessor-like lines.
bool sourceDialect(Dialect dialect) {
  return dialect == Dialect::C || dialect == Dialect::Rust ||
         dialect == Dialect::Go;
}
const QSet<QString> &llvmWords() {
  static const QSet<QString> words = {
#define NEVERD_LLVM_KEYWORD(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
  };
  return words;
}

bool identifierStart(QChar c) { return c.isLetter() || c == QLatin1Char('_'); }
bool identifierPart(QChar c) {
  return c.isLetterOrNumber() || c == QLatin1Char('_');
}
} // namespace

CodeText::CodeText(Session &session, QWidget *parent)
    : QAbstractScrollArea(parent), session_(session) {
  setFocusPolicy(Qt::StrongFocus);
  setFrameShape(QFrame::NoFrame);
  viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
  viewport()->setCursor(Qt::IBeamCursor);
  updateMetrics();
  connect(&Theme::instance(), &Theme::changed, this, [this] {
    ++styleStamp_;
    updateMetrics();
    viewport()->update();
  });
  connect(&session_, &Session::revisionChanged, this, [this] {
    if (function_)
      load(*function_, representation_);
  });
  connect(&session_, &Session::unloaded, this, &CodeText::clear);
}

void CodeText::updateMetrics() {
  const QFontMetricsF metrics(Theme::instance().codeFont());
  charWidth_ = metrics.horizontalAdvance(QLatin1Char('M'));
  lineHeight_ = int(std::ceil(metrics.lineSpacing()));
  ascent_ = int(std::ceil(metrics.ascent()));
  updateRange();
}

int CodeText::visibleLines() const {
  return std::max(1, viewport()->height() / std::max(1, lineHeight_));
}

void CodeText::updateRange() {
  verticalScrollBar()->setRange(
      0, std::max(0, int(lines_.size()) - visibleLines() + 1));
  verticalScrollBar()->setPageStep(visibleLines());
  int widest = 0;
  for (const auto &line : lines_)
    widest = std::max(widest, int(line.styled.text.size()));
  horizontalScrollBar()->setRange(
      0,
      std::max(0, int(widest * charWidth_) + textLeft() - viewport()->width()));
  horizontalScrollBar()->setPageStep(viewport()->width());
}

void CodeText::resizeEvent(QResizeEvent *event) {
  QAbstractScrollArea::resizeEvent(event);
  updateRange();
}

void CodeText::scrollContentsBy(int, int) { viewport()->update(); }

void CodeText::clear() {
  ++serial_;
  loading_ = false;
  source_.clear();
  sourceRows_.clear();
  lineStarts_.clear();
  regions_ = {};
  regionsValid_ = false;
  prelude_ = {};
  declarations_.reset();
  library_.reset({}, {}, {});
  lines_.clear();
  function_.reset();
  status_.clear();
  cursorLine_ = cursorColumn_ = 0;
  anchor_.reset();
  marked_.clear();
  updateRange();
  viewport()->update();
  emit statusChanged();
}

void CodeText::load(Address function, const QString &representation) {
  const bool same = function_ == function && representation_ == representation;
  function_ = function;
  representation_ = representation;
  const quint64 serial = ++serial_;
  if (!same) {
    lines_.clear();
    cursorLine_ = cursorColumn_ = 0;
    anchor_.reset();
    verticalScrollBar()->setValue(0);
  }
  inComment_ = false;
  loading_ = true;
  // Folding state survives a refresh of the same function as "fold all";
  // a new function opens at its definition, its prelude folded.
  foldAfterLoad_ = same && library_.libraryFolded();
  foldPreludeAfterLoad_ = !same || library_.isFolded(PreludeRegion);
  library_.reset({}, {}, {});
  emit foldingChanged();
  status_ = tr("Decompiling…");
  emit statusChanged();
  request(0, serial);
}

void CodeText::request(int offset, quint64 serial) {
  if (!function_)
    return;
  session_.read(
      QStringLiteral("decompile"),
      {{"address", hexAddress(*function_)},
       {"representation", representation_},
       {"offset", offset},
       {"limit", PageLines}},
      this,
      [this, offset, serial](const QJsonObject &payload) {
        if (serial != serial_)
          return;
        appendPage(payload, offset);
        const auto next = payload.value("next_offset");
        if (!next.isNull() && next.toInt() > offset) {
          rebuildLines();
          request(next.toInt(), serial);
          return;
        }
        loading_ = false;
        // Folding needs the whole function: regions span pages.
        library_.reset(source_, sourceRows_,
                       regionsValid_ ? foldRegions() : QJsonArray(),
                       byteOffset_);
        if (foldAfterLoad_)
          library_.setFolded(true);
        if (foldPreludeAfterLoad_)
          library_.setRegionFolded(PreludeRegion, true);
        rebuildLines();
        emit foldingChanged();
        const auto mapping = payload.value("mapping_status").toString();
        status_ = session_.metadata().value("analyzed").toBool()
                      ? QString()
                      : tr("Function-level analysis");
        if (mapping == QLatin1String("instruction_anchors"))
          status_ += (status_.isEmpty() ? QString() : QStringLiteral(" · ")) +
                     tr("rows linked to instructions");
        if (unread_ > 0)
          status_ += (status_.isEmpty() ? QString() : QStringLiteral(" · ")) +
                     tr("%n declarations shown as C", nullptr, unread_);
        updateRange();
        viewport()->update();
        emit statusChanged();
      },
      [this, serial](const QString &, const QString &message) {
        if (serial != serial_)
          return;
        loading_ = false;
        lines_.clear();
        Line line;
        line.styled.text = message;
        line.styled.spans = {{0, int(message.size()), Comment, std::nullopt}};
        lines_.append(std::move(line));
        status_ = message;
        updateRange();
        viewport()->update();
        emit statusChanged();
      });
}

void CodeText::appendPage(const QJsonObject &payload, int offset) {
  const QString text = payload.value("text").toString();
  const qint64 pageOffset = payload.value("byte_offset").toInteger(-1);
  if (offset == 0) {
    source_.clear();
    sourceRows_.clear();
    regions_ = payload.value("library_regions").toArray();
    regionsValid_ =
        payload.value("library_regions").isArray() && pageOffset >= 0;
    prelude_ = payload.value("prelude").toObject();
    declarations_.reset();
    byteOffset_ = std::max<qint64>(0, pageOffset);
    sourceNames_.clear();
    sourceNameIndex_.clear();
    unread_ = int(payload.value("unread").toArray().size());
    const QString language = payload.value("dialect").toString();
    if (language != pageLanguage_) {
      pageLanguage_ = language;
      emit languageChanged();
    }
  } else if (regionsValid_ &&
             pageOffset != byteOffset_ + source_.toUtf8().size()) {
    // Pages that do not continue each other cannot share byte offsets.
    regionsValid_ = false;
  }
  source_ += text;
  if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n')))
    source_ += QLatin1Char('\n');
  for (const auto &row : payload.value("rows").toArray())
    sourceRows_.append(row.toObject().toVariantMap());
  // A source name's bytes are in the page that holds its line.
  const QByteArray bytes = text.toUtf8();
  bool namesAdded = false;
  for (const auto &value : payload.value("source_names").toArray()) {
    const auto name = value.toObject();
    const qint64 begin = name.value("begin_byte").toInteger(-1) - pageOffset;
    const qint64 end = name.value("end_byte").toInteger(-1) - pageOffset;
    if (pageOffset < 0 || begin < 0 || end <= begin || end > bytes.size())
      continue;
    const QString spelled = QString::fromUtf8(bytes.mid(begin, end - begin));
    if (spelled.isEmpty() || sourceNames_.contains(spelled))
      continue;
    sourceNames_.insert(spelled, {name.value("identifier").toString(),
                                  name.value("symbol").toString(),
                                  addressValue(name.value("address"))});
    auto &bucket = sourceNameIndex_[spelled.at(0)];
    bucket.append(spelled);
    namesAdded = true;
  }
  if (namesAdded)
    for (auto &bucket : sourceNameIndex_)
      std::sort(bucket.begin(), bucket.end(),
                [](const QString &a, const QString &b) {
                  return a.size() > b.size();
                });
}

QString CodeText::language() const {
  switch (dialectOf(representation_, pageLanguage_)) {
  case Dialect::C:
    return QStringLiteral("c");
  case Dialect::Rust:
    return QStringLiteral("rust");
  case Dialect::Go:
    return QStringLiteral("go");
  default:
    return {};
  }
}

int CodeText::sourceNameLength(const QString &text, int position) const {
  const auto bucket = sourceNameIndex_.constFind(text.at(position));
  if (bucket == sourceNameIndex_.cend())
    return 0;
  // A name starts and ends where identifiers do, so `core::fmt::write` is
  // not found inside `xcore::fmt::writer`.
  const auto joins = [](QChar c) {
    return identifierPart(c) || c == QLatin1Char(':') ||
           c == QLatin1Char('.') || c == QLatin1Char('/');
  };
  if (position > 0 && joins(text.at(position - 1)))
    return 0;
  for (const QString &name : *bucket) {
    const int end = position + int(name.size());
    if (end <= text.size() &&
        QStringView(text).mid(position, name.size()) == name &&
        (end == text.size() || !identifierPart(text.at(end))))
      return int(name.size());
  }
  return 0;
}

std::optional<std::pair<QString, CodeText::SourceName>>
CodeText::sourceNameAt(int line, int column) const {
  if (line < 0 || line >= lines_.size() || sourceNames_.isEmpty())
    return std::nullopt;
  const auto &styled = lines_[line].styled;
  for (const auto &span : styled.spans) {
    if (span.role != Function || column < span.start ||
        column >= span.start + span.length)
      continue;
    const QString spelled = styled.text.mid(span.start, span.length);
    if (const auto found = sourceNames_.constFind(spelled);
        found != sourceNames_.cend())
      return std::pair{spelled, *found};
  }
  return std::nullopt;
}

QJsonArray CodeText::foldRegions() const {
  QJsonArray regions = regions_;
  // The prelude folds to its summary line, the newline before the
  // definition kept.
  const qint64 lines = prelude_.value("lines").toInteger(-1);
  const qint64 end = prelude_.value("end_byte").toInteger(-1);
  if (lines > 0 && end > byteOffset_ + 1)
    regions.append(QJsonObject{
        {"id", PreludeRegion},
        {"kind", PreludeRegion},
        {"display_name",
         tr("%n lines of includes and declarations", nullptr, int(lines))},
        {"foldable", true},
        {"mapping_status", "mapped"},
        {"spans", QJsonArray{QJsonObject{{"begin_byte", byteOffset_},
                                         {"end_byte", end - 1}}}}});
  return regions;
}

void CodeText::rebuildLines() {
  const bool folding = !loading_ && library_.canFold();
  const QString display = folding ? library_.text() : source_;
  const QVariantList &rows = folding ? library_.mappings() : sourceRows_;
  const int previousLine = cursorLine_;
  lines_.clear();
  lineStarts_.clear();
  inComment_ = false;
  int start = 0;
  while (start < display.size()) {
    int end = int(display.indexOf(QLatin1Char('\n'), start));
    if (end < 0)
      end = int(display.size());
    Line line;
    line.styled.text = display.mid(start, end - start);
    highlightLine(line, inComment_);
    lines_.append(std::move(line));
    lineStarts_.append(start);
    start = end + 1;
  }
  for (const auto &value : rows) {
    const auto row = value.toMap();
    const int index = row.value(QStringLiteral("line"), -1).toInt();
    if (index < 0 || index >= lines_.size())
      continue;
    for (const auto &address : row.value(QStringLiteral("addresses")).toList())
      if (const auto parsed = parseAddress(address.toString()))
        lines_[index].addresses.append(*parsed);
  }
  if (folding)
    for (const auto &[begin, end] : library_.foldedRanges()) {
      auto it =
          std::upper_bound(lineStarts_.cbegin(), lineStarts_.cend(), begin);
      const int index = int(it - lineStarts_.cbegin()) - 1;
      if (index < 0 || index >= lines_.size())
        continue;
      auto &line = lines_[index];
      line.folded = true;
      const int column = begin - lineStarts_[index];
      const int length =
          std::min(end, int(lineStarts_[index] + line.styled.text.size())) -
          begin;
      // The summary reads as a comment over the fold tint.
      QVector<StyledSpan> spans;
      for (const auto &span : line.styled.spans)
        if (span.start + span.length <= column || span.start >= column + length)
          spans.append(span);
      spans.append({column, length, Comment, std::nullopt});
      std::sort(spans.begin(), spans.end(),
                [](const StyledSpan &a, const StyledSpan &b) {
                  return a.start < b.start;
                });
      line.styled.spans = spans;
    }
  gutterChars_ = std::max(3, int(QString::number(lines_.size()).size()));
  cursorLine_ =
      std::clamp(previousLine, 0, std::max(0, int(lines_.size()) - 1));
  updateRange();
  viewport()->update();
}

int CodeText::displayPosition(int line, int column) const {
  if (line < 0 || line >= lineStarts_.size())
    return 0;
  return lineStarts_[line] + column;
}

QString CodeText::regionAt(const QPoint &position) const {
  if (!library_.anyFolded())
    return {};
  const int line = lineAt(position.y());
  if (line < 0 || line >= lines_.size() || !lines_[line].folded)
    return {};
  return library_.regionAt(displayPosition(line, columnAt(line, position.x())));
}

void CodeText::setFolded(bool folded) {
  library_.setFolded(folded);
  rebuildLines();
  emit foldingChanged();
}

bool CodeText::event(QEvent *event) {
  if (event->type() == QEvent::ToolTip) {
    auto *help = static_cast<QHelpEvent *>(event);
    const QPoint local = viewport()->mapFrom(this, help->pos());
    if (const QString id = regionAt(local); !id.isEmpty()) {
      for (const auto &value : library_.regions()) {
        const auto region = value.toMap();
        if (region.value(QStringLiteral("id")).toString() != id)
          continue;
        QToolTip::showText(
            help->globalPos(),
            id == QLatin1String(PreludeRegion)
                ? tr("The includes, support types and declarations this code "
                     "compiles with; click to show them, Keypad - to fold "
                     "them again.")
                : tr("%1\nRecognized library operation; click to show its "
                     "code.")
                      .arg(region.value(QStringLiteral("display_name"))
                               .toString()),
            this);
        return true;
      }
    }
    // A source name shows the C identifier and the symbol it reads.
    if (const int line = lineAt(local.y()); line >= 0 && line < lines_.size())
      if (const auto name = sourceNameAt(line, columnAt(line, local.x()))) {
        QString text = tr("C: %1").arg(name->second.identifier);
        if (!name->second.symbol.isEmpty() &&
            name->second.symbol != name->first)
          text += QLatin1Char('\n') + name->second.symbol;
        QToolTip::showText(help->globalPos(), text, this);
        return true;
      }
    // A type or macro the code declares shows its declaration.
    if (const int line = lineAt(local.y()); line >= 0 && line < lines_.size())
      if (const QString token =
              lines_[line].styled.tokenAt(columnAt(line, local.x()));
          !token.isEmpty())
        if (const auto declared = declarationLine(token)) {
          const auto text = QStringView(source_)
                                .split(QLatin1Char('\n'))
                                .at(*declared)
                                .trimmed();
          QToolTip::showText(help->globalPos(),
                             tr("%1\nDouble-click to go to the declaration.")
                                 .arg(text.toString()),
                             this);
          return true;
        } else if (const auto linked = declarations().linked.constFind(token);
                   linked != declarations().linked.cend()) {
          // Its declaration names the symbol and the demangled signature.
          QToolTip::showText(help->globalPos(),
                             QStringView(source_)
                                 .split(QLatin1Char('\n'))
                                 .at(linked->first)
                                 .trimmed()
                                 .toString(),
                             this);
          return true;
        }
    QToolTip::hideText();
    event->ignore();
    return true;
  }
  return QAbstractScrollArea::event(event);
}

void CodeText::highlightLine(Line &line, bool &inComment) const {
  const Dialect dialect = dialectOf(representation_, pageLanguage_);
  const bool source = sourceDialect(dialect);
  const QString &s = line.styled.text;
  auto &spans = line.styled.spans;
  spans.clear();
  const auto add = [&](int start, int end, int role) {
    if (end > start)
      spans.append({start, end - start, role, std::nullopt});
  };
  int i = 0;
  const int n = int(s.size());
  if (source && !inComment) {
    int first = 0;
    while (first < n && s.at(first).isSpace())
      ++first;
    if (first < n && s.at(first) == QLatin1Char('#')) {
      add(first, n, Preprocessor);
      return;
    }
  }
  while (i < n) {
    const QChar c = s.at(i);
    if (inComment) {
      const int end = int(s.indexOf(QStringLiteral("*/"), i));
      if (end < 0) {
        add(i, n, Comment);
        return;
      }
      add(i, end + 2, Comment);
      i = end + 2;
      inComment = false;
      continue;
    }
    if (source && c == QLatin1Char('/') && i + 1 < n &&
        s.at(i + 1) == QLatin1Char('/')) {
      add(i, n, Comment);
      return;
    }
    if (source && c == QLatin1Char('/') && i + 1 < n &&
        s.at(i + 1) == QLatin1Char('*')) {
      inComment = true;
      continue;
    }
    if (!source && c == QLatin1Char(';')) {
      add(i, n, Comment);
      return;
    }
    // A Rust label, `'switch1`, is not a character literal.
    if (dialect == Dialect::Rust && c == QLatin1Char('\'') && i + 1 < n &&
        identifierStart(s.at(i + 1))) {
      int j = i + 2;
      while (j < n && identifierPart(s.at(j)))
        ++j;
      if (j >= n || s.at(j) != QLatin1Char('\'')) {
        add(i, j, Constant);
        i = j;
        continue;
      }
    }
    // A source name, `core::fmt::write`, is one name.
    if (!sourceNames_.isEmpty() && source) {
      if (const int length = sourceNameLength(s, i)) {
        const auto name = sourceNames_.value(s.mid(i, length));
        spans.append({i, length, Function, name.address});
        i += length;
        continue;
      }
    }
    if (c == QLatin1Char('"') || (source && c == QLatin1Char('\''))) {
      int j = i + 1;
      while (j < n && s.at(j) != c) {
        if (s.at(j) == QLatin1Char('\\'))
          ++j;
        ++j;
      }
      add(i, std::min(n, j + 1), String);
      i = std::min(n, j + 1);
      continue;
    }
    if (c.isDigit()) {
      int j = i + 1;
      while (j < n &&
             (s.at(j).isLetterOrNumber() || s.at(j) == QLatin1Char('.') ||
              s.at(j) == QLatin1Char('_')))
        ++j;
      add(i, j, Number);
      i = j;
      continue;
    }
    const bool sigil = dialect == Dialect::LLVM &&
                       (c == QLatin1Char('%') || c == QLatin1Char('@'));
    if (identifierStart(c) || sigil) {
      int j = i + 1;
      while (j < n && (identifierPart(s.at(j)) || s.at(j) == QLatin1Char('.')))
        ++j;
      const QString word = s.mid(i, j - i);
      int k = j;
      while (k < n && s.at(k) == QLatin1Char(' '))
        ++k;
      const bool call = k < n && s.at(k) == QLatin1Char('(');
      int role = Variable;
      if (source) {
        const auto &words = vocabularyOf(dialect);
        if (words.control.contains(word))
          role = Control;
        else if (words.types.contains(word) || widthType(dialect, word) ||
                 (dialect == Dialect::C && word.endsWith(QStringLiteral("_t"))))
          role = Type;
        else if (words.keywords.contains(word))
          role = Keyword;
        else if (call)
          role = Function;
        else if (word.size() > 1 && word == word.toUpper() &&
                 word.at(0).isLetter())
          role = Constant;
      } else if (dialect == Dialect::LLVM) {
        if (c == QLatin1Char('@'))
          role = Function;
        else if (c == QLatin1Char('%'))
          role = Variable;
        else if (llvmWords().contains(word))
          role = Keyword;
        else if (word.size() > 1 && word.at(0) == QLatin1Char('i') &&
                 word.mid(1).toInt() > 0)
          role = Type;
        else
          role = Default;
      } else {
        // Textual IR: OPCODES in capitals, labels end with a colon.
        if (word == word.toUpper() && word.size() > 1)
          role = Keyword;
        else if (k < n && s.at(k) == QLatin1Char(':'))
          role = Function;
        else
          role = Variable;
      }
      add(i, j, role);
      i = j;
      continue;
    }
    if (!c.isSpace())
      add(i, i + 1, Punctuation);
    ++i;
  }
}

int CodeText::textLeft() const {
  return Margin + int((gutterChars_ + 2) * charWidth_) -
         horizontalScrollBar()->value();
}

int CodeText::lineAt(int y) const {
  return verticalScrollBar()->value() + y / std::max(1, lineHeight_);
}

int CodeText::columnAt(int line, int x) const {
  if (line < 0 || line >= lines_.size())
    return 0;
  auto &layout = lines_[line].styled.layout(Theme::instance().codeFont(),
                                            styleStamp_, codeColor);
  return layout.lineCount() ? layout.lineAt(0).xToCursor(x - textLeft()) : 0;
}

void CodeText::paintEvent(QPaintEvent *) {
  QPainter painter(viewport());
  const auto &theme = Theme::instance();
  const QRect area = viewport()->rect();
  painter.fillRect(area, theme.color(ColorRole::CodeBackground));
  if (lines_.isEmpty())
    return;
  const QFont font = theme.codeFont();
  painter.setFont(font);
  const int first = verticalScrollBar()->value();
  const int last = std::min(int(lines_.size()) - 1, first + visibleLines());
  const int left = textLeft();
  const int selectionFirst =
      anchor_ ? std::min(anchor_->first, cursorLine_) : -1;
  const int selectionLast =
      anchor_ ? std::max(anchor_->first, cursorLine_) : -1;
  for (int i = first; i <= last; ++i) {
    const int y = (i - first) * lineHeight_;
    const QRect row(0, y, area.width(), lineHeight_);
    if (i >= selectionFirst && i <= selectionLast)
      painter.fillRect(row, theme.color(ColorRole::ListingSelection));
    else if (i == cursorLine_)
      painter.fillRect(row, theme.color(ColorRole::ListingCurrentLine));
    else if (marked_.contains(i) || lines_[i].folded)
      painter.fillRect(row, theme.color(ColorRole::CodeFold));
    painter.setPen(theme.color(ColorRole::CodeLineNumber));
    painter.drawText(QRect(Margin - horizontalScrollBar()->value(), y,
                           int(gutterChars_ * charWidth_), lineHeight_),
                     Qt::AlignRight | Qt::AlignVCenter, QString::number(i + 1));
    auto &layout = lines_[i].styled.layout(font, styleStamp_, codeColor);
    if (!highlight_.isEmpty() && layout.lineCount()) {
      const QTextLine textLine = layout.lineAt(0);
      for (const int at : tokenOccurrences(lines_[i].styled.text, highlight_)) {
        const qreal x0 = textLine.cursorToX(at);
        const qreal x1 = textLine.cursorToX(at + int(highlight_.size()));
        painter.fillRect(QRectF(left + x0, y, x1 - x0, lineHeight_),
                         theme.color(ColorRole::ListingHighlight));
      }
    }
    layout.draw(&painter, QPointF(left, y));
    if (i == cursorLine_ && hasFocus() && layout.lineCount()) {
      const qreal x = left + layout.lineAt(0).cursorToX(cursorColumn_);
      painter.fillRect(QRectF(x, y + 1, 2, lineHeight_ - 2),
                       theme.color(ColorRole::ListingCursor));
    }
  }
}

void CodeText::moveCursor(int line, int column, bool extend) {
  if (lines_.isEmpty())
    return;
  if (extend && !anchor_)
    anchor_ = std::pair{cursorLine_, cursorColumn_};
  else if (!extend)
    anchor_.reset();
  cursorLine_ = std::clamp(line, 0, int(lines_.size()) - 1);
  cursorColumn_ =
      std::clamp(column, 0, int(lines_[cursorLine_].styled.text.size()));
  auto *bar = verticalScrollBar();
  if (cursorLine_ < bar->value())
    bar->setValue(cursorLine_);
  else if (cursorLine_ >= bar->value() + visibleLines())
    bar->setValue(cursorLine_ - visibleLines() + 1);
  if (const auto address = currentAddress())
    emit locationChanged(*address);
  viewport()->update();
}

std::optional<Address> CodeText::currentAddress() const {
  if (cursorLine_ < 0 || cursorLine_ >= lines_.size() ||
      lines_[cursorLine_].addresses.isEmpty())
    return std::nullopt;
  return lines_[cursorLine_].addresses.front();
}

void CodeText::revealAddress(Address address) {
  marked_.clear();
  for (int i = 0; i < lines_.size(); ++i)
    if (lines_[i].addresses.contains(address))
      marked_.append(i);
  if (!marked_.isEmpty()) {
    auto *bar = verticalScrollBar();
    const int line = marked_.front();
    if (line < bar->value() || line >= bar->value() + visibleLines())
      bar->setValue(std::max(0, line - visibleLines() / 3));
  }
  viewport()->update();
}

QString CodeText::currentToken() const {
  if (cursorLine_ < 0 || cursorLine_ >= lines_.size())
    return {};
  return lines_[cursorLine_].styled.tokenAt(cursorColumn_);
}

QString CodeText::selectedText() const {
  if (lines_.isEmpty())
    return {};
  const int first = anchor_
                        ? std::min(anchor_->first, cursorLine_)
                        : std::clamp(cursorLine_, 0, int(lines_.size()) - 1);
  const int last = anchor_ ? std::max(anchor_->first, cursorLine_) : first;
  // Folded summaries copy as the code they stand for.
  if (library_.anyFolded()) {
    const int begin = displayPosition(first, 0);
    const int end = displayPosition(last, int(lines_[last].styled.text.size()));
    return library_.originalSelection(begin, end);
  }
  QStringList text;
  for (int i = first; i <= last && i < lines_.size(); ++i)
    text.append(lines_[i].styled.text);
  return text.join(QLatin1Char('\n'));
}

QString CodeText::allText() const {
  QString text = source_;
  if (text.endsWith(QLatin1Char('\n')))
    text.chop(1);
  return text;
}

bool CodeText::findText(const QString &text, bool forward) {
  if (text.isEmpty())
    return false;
  for (int step = 1; step <= lines_.size(); ++step) {
    const int index = forward ? cursorLine_ + step : cursorLine_ - step;
    if (index < 0 || index >= lines_.size())
      break;
    const int column =
        int(lines_[index].styled.text.indexOf(text, 0, Qt::CaseInsensitive));
    if (column >= 0) {
      highlight_ = text;
      moveCursor(index, column, false);
      return true;
    }
  }
  return false;
}

void CodeText::keyPressEvent(QKeyEvent *event) {
  const bool shift = event->modifiers() & Qt::ShiftModifier;
  switch (event->key()) {
  case Qt::Key_Up:
    moveCursor(cursorLine_ - 1, cursorColumn_, shift);
    return;
  case Qt::Key_Down:
    moveCursor(cursorLine_ + 1, cursorColumn_, shift);
    return;
  case Qt::Key_PageUp:
    moveCursor(cursorLine_ - visibleLines() + 1, cursorColumn_, shift);
    return;
  case Qt::Key_PageDown:
    moveCursor(cursorLine_ + visibleLines() - 1, cursorColumn_, shift);
    return;
  case Qt::Key_Left:
    moveCursor(cursorLine_, cursorColumn_ - 1, shift);
    return;
  case Qt::Key_Right:
    moveCursor(cursorLine_, cursorColumn_ + 1, shift);
    return;
  case Qt::Key_Home:
    moveCursor((event->modifiers() & Qt::ControlModifier) ? 0 : cursorLine_, 0,
               shift);
    return;
  case Qt::Key_End:
    if (event->modifiers() & Qt::ControlModifier)
      moveCursor(int(lines_.size()) - 1, 0, shift);
    else if (cursorLine_ < lines_.size())
      moveCursor(cursorLine_, int(lines_[cursorLine_].styled.text.size()),
                 shift);
    return;
  default:
    break;
  }
  if (event->matches(QKeySequence::Copy)) {
    QApplication::clipboard()->setText(selectedText());
    return;
  }
  // Keypad + shows the folded code at the cursor and Keypad - folds the
  // prelude back, as the decompiler views of classic disassemblers do.
  if (event->modifiers() & Qt::KeypadModifier) {
    if (event->key() == Qt::Key_Plus) {
      if (const QString id =
              library_.regionAt(displayPosition(cursorLine_, cursorColumn_));
          !id.isEmpty()) {
        library_.setRegionFolded(id, false);
        rebuildLines();
        emit foldingChanged();
      }
      return;
    }
    if (event->key() == Qt::Key_Minus) {
      if (hasPrelude() && !preludeFolded() &&
          cursorLine_ < prelude_.value("lines").toInteger(0))
        setPreludeFolded(true);
      return;
    }
  }
  QAbstractScrollArea::keyPressEvent(event);
}

void CodeText::mousePressEvent(QMouseEvent *event) {
  setFocus(Qt::MouseFocusReason);
  if (event->button() == Qt::LeftButton)
    if (const QString id = regionAt(event->position().toPoint());
        !id.isEmpty()) {
      library_.toggleRegion(id);
      rebuildLines();
      emit foldingChanged();
      return;
    }
  const int line =
      std::min(lineAt(int(event->position().y())), int(lines_.size()) - 1);
  if (line < 0)
    return;
  const int column = columnAt(line, int(event->position().x()));
  moveCursor(line, column, event->modifiers() & Qt::ShiftModifier);
  if (event->button() == Qt::LeftButton) {
    const auto name = sourceNameAt(line, column);
    highlight_ = name ? name->first : lines_[line].styled.tokenAt(column);
  }
  viewport()->update();
}

void CodeText::mouseMoveEvent(QMouseEvent *event) {
  if (!(event->buttons() & Qt::LeftButton) || lines_.isEmpty())
    return;
  const int line =
      std::clamp(lineAt(int(event->position().y())), 0, int(lines_.size()) - 1);
  if (!anchor_)
    anchor_ = std::pair{cursorLine_, cursorColumn_};
  cursorLine_ = line;
  cursorColumn_ = columnAt(line, int(event->position().x()));
  viewport()->update();
}

void CodeText::mouseDoubleClickEvent(QMouseEvent *event) {
  if (event->button() != Qt::LeftButton)
    return;
  // A source name resolves by its symbol, as the C view's linked names do.
  if (const auto name = sourceNameAt(cursorLine_, cursorColumn_)) {
    emit nameActivated(name->second.symbol.isEmpty() ? name->second.identifier
                                                     : name->second.symbol);
    return;
  }
  const QString token = currentToken();
  // A type or macro this code declares opens at its declaration; the
  // language's own words name nothing in the binary.
  const auto &words = vocabularyOf(dialectOf(representation_, pageLanguage_));
  if (token.isEmpty() || goToDeclaration(token) ||
      words.control.contains(token) || words.keywords.contains(token) ||
      words.types.contains(token))
    return;
  // A global shows at its address, whatever C called it there.
  if (const auto address = objectAddress(token)) {
    emit objectActivated(*address);
    return;
  }
  // A C++ function links by its mangled symbol, which the binary names.
  emit nameActivated(linkedSymbol(token).value_or(token));
}

bool CodeText::hasPrelude() const {
  return library_.hasRegion(QLatin1String(PreludeRegion));
}

bool CodeText::preludeFolded() const {
  return library_.isFolded(QLatin1String(PreludeRegion));
}

void CodeText::setPreludeFolded(bool folded) {
  if (!hasPrelude() || preludeFolded() == folded)
    return;
  library_.setRegionFolded(QLatin1String(PreludeRegion), folded);
  rebuildLines();
  if (folded)
    moveCursor(0, 0, false);
  emit foldingChanged();
}

const CodeText::Declarations &CodeText::declarations() const {
  if (declarations_)
    return *declarations_;
  // The declared name ends a typedef, before its attributes; a record or
  // macro names itself first; a labeled function names itself before its
  // parameters.
  static const QRegularExpression Typedef(QStringLiteral(
      R"(^\s*typedef\b.*?\b([A-Za-z_]\w*)\s*(?:__attribute__\s*\(\(.*\)\)\s*)?;\s*$)"));
  static const QRegularExpression Record(QStringLiteral(
      R"(^\s*(?:typedef\s+)?(?:struct|union|enum)\s+([A-Za-z_]\w*)\s*\{)"));
  static const QRegularExpression Define(
      QStringLiteral(R"(^\s*#\s*define\s+([A-Za-z_]\w*))"));
  static const QRegularExpression Labeled(QStringLiteral(
      R"re(\b([A-Za-z_]\w*)\s*\([^;]*\)\s*__asm__\s*\(\s*"([^"\\]+)"\s*\))re"));
  // A global's address: in the comment above its declaration (decompiled C)
  // or after it (C through LLVM).  The declared name precedes its array
  // bounds and its initializer or the end of the declaration.
  static const QRegularExpression ImageAddress(
      QStringLiteral(R"(^\s*/\*\s*neverd\.image:\s*0x([0-9A-Fa-f]+)\b)"));
  static const QRegularExpression TrailingAddress(
      QStringLiteral(R"(;\s*/\*\s*0x([0-9A-Fa-f]+)\s*\*/\s*$)"));
  static const QRegularExpression Declared(
      QStringLiteral(R"(\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*[=;])"));
  auto &found = declarations_.emplace();
  int line = 0;
  std::optional<Address> declaredAt;
  for (const QStringView text : QStringView(source_).split(QLatin1Char('\n'))) {
    const QStringView head = text.trimmed();
    if (const auto marker = ImageAddress.matchView(text); marker.hasMatch()) {
      declaredAt = marker.captured(1).toULongLong(nullptr, 16);
      ++line;
      continue;
    }
    std::optional<Address> address = std::exchange(declaredAt, std::nullopt);
    if (const auto trailing = TrailingAddress.matchView(text);
        !address && trailing.hasMatch() && head.startsWith(u"extern"))
      address = trailing.captured(1).toULongLong(nullptr, 16);
    if (address)
      if (const auto name = Declared.matchView(text);
          name.hasMatch() && !found.objects.contains(name.captured(1)))
        found.objects.insert(name.captured(1), *address);
    if (head.startsWith(u"typedef") || head.startsWith(u"struct") ||
        head.startsWith(u"union") || head.startsWith(u"enum") ||
        head.startsWith(u'#')) {
      for (const auto *pattern : {&Typedef, &Record, &Define})
        if (const auto match = pattern->matchView(text); match.hasMatch()) {
          if (!found.types.contains(match.captured(1)))
            found.types.insert(match.captured(1), line);
          break;
        }
    } else if (text.contains(u"__asm__")) {
      if (const auto match = Labeled.matchView(text);
          match.hasMatch() && !found.linked.contains(match.captured(1)))
        found.linked.insert(match.captured(1), {line, match.captured(2)});
    }
    ++line;
  }
  return found;
}

std::optional<int> CodeText::declarationLine(const QString &name) const {
  const auto &types = declarations().types;
  if (const auto found = types.constFind(name); found != types.cend())
    return *found;
  return std::nullopt;
}

std::optional<Address> CodeText::objectAddress(const QString &name) const {
  const auto &objects = declarations().objects;
  if (const auto found = objects.constFind(name); found != objects.cend())
    return *found;
  return std::nullopt;
}

std::optional<QString> CodeText::linkedSymbol(const QString &name) const {
  const auto &linked = declarations().linked;
  if (const auto found = linked.constFind(name); found != linked.cend())
    return found->second;
  return std::nullopt;
}

bool CodeText::goToDeclaration(const QString &name) {
  const auto source = declarationLine(name);
  if (!source || loading_)
    return false;
  if (hasPrelude() && preludeFolded() &&
      *source < prelude_.value("lines").toInteger(0))
    setPreludeFolded(false);
  const int line = library_.canFold() ? library_.displayLine(*source) : *source;
  if (line < 0 || line >= lines_.size())
    return false;
  highlight_ = name;
  moveCursor(line, std::max(0, int(lines_[line].styled.text.indexOf(name))),
             false);
  return true;
}

void CodeText::wheelEvent(QWheelEvent *event) {
  if (event->modifiers() & Qt::ControlModifier) {
    Theme::instance().zoomCodeFont(event->angleDelta().y() > 0 ? 1 : -1);
    return;
  }
  const int steps = event->angleDelta().y() / 120;
  verticalScrollBar()->setValue(verticalScrollBar()->value() -
                                steps * WheelLines);
}

void CodeText::contextMenuEvent(QContextMenuEvent *event) {
  emit contextMenuRequested(event->globalPos());
}

//===----------------------------------------------------------------------===//
// CodeView
//===----------------------------------------------------------------------===//

QString CodeView::titleOf(const QString &representation) {
  if (const auto *entry = representationOf(representation))
    return QCoreApplication::translate("Representations", entry->title);
  return representation;
}

bool CodeView::isSource(const QString &representation) {
  const auto *entry = representationOf(representation);
  return entry &&
         (sourceDialect(entry->dialect) || entry->dialect == Dialect::Source);
}

QString CodeView::chosenLanguage() const {
  const auto *entry = representationOf(representation());
  if (!entry || entry->dialect != Dialect::Source ||
      text_->representation() != representation())
    return {};
  // Language names are the same in every translation; C, the common case,
  // goes without saying.
  const QString language = text_->language();
  if (language == QLatin1String("rust"))
    return QStringLiteral("Rust");
  if (language == QLatin1String("go"))
    return QStringLiteral("Go");
  return {};
}

CodeView::CodeView(Session &session, const QString &representation,
                   QWidget *parent)
    : QWidget(parent), session_(session), selector_(new QComboBox(this)),
      fold_(new QToolButton(this)), lock_(new QToolButton(this)),
      status_(new QLabel(this)), text_(new CodeText(session, this)) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  auto *bar = new QHBoxLayout;
  bar->setContentsMargins(4, 2, 4, 2);
  for (const auto &entry : Representations)
    selector_->addItem(
        QCoreApplication::translate("Representations", entry.title),
        QString::fromLatin1(entry.name));
  selector_->setCurrentIndex(selector_->findData(representation));
  bar->addWidget(selector_);
  fold_->setCheckable(true);
  fold_->setAutoRaise(true);
  fold_->setIcon(icon(QStringLiteral("function_library")));
  fold_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  fold_->setText(tr("Fold library code"));
  fold_->setToolTip(tr("Show each recognized library operation as a one-line "
                       "summary; copy and export keep the full code."));
  fold_->setVisible(false);
  bar->addWidget(fold_);
  lock_->setCheckable(true);
  lock_->setAutoRaise(true);
  lock_->setIcon(icon(QStringLiteral("lock")));
  lock_->setToolTip(tr("Keep this function while the disassembly moves on"));
  bar->addWidget(lock_);
  bar->addStretch(1);
  layout->addLayout(bar);
  layout->addWidget(text_, 1);
  status_->setContentsMargins(6, 2, 6, 2);
  layout->addWidget(status_);
  connect(selector_, &QComboBox::currentIndexChanged, this, [this] {
    const auto name = selector_->currentData().toString();
    if (text_->function())
      text_->load(*text_->function(), name);
    emit representationChanged(name);
  });
  connect(text_, &CodeText::statusChanged, this, &CodeView::updateStatus);
  connect(text_, &CodeText::languageChanged, this, &CodeView::languageChanged);
  connect(fold_, &QToolButton::toggled, text_, &CodeText::setFolded);
  connect(text_, &CodeText::foldingChanged, this, [this] {
    fold_->setVisible(text_->foldableCount() > 0);
    const QSignalBlocker blocker(fold_);
    fold_->setChecked(text_->libraryFolded());
  });
}

void CodeView::showFunction(Address function) {
  text_->load(function, selector_->currentData().toString());
}

void CodeView::setRepresentation(const QString &representation) {
  const int index = selector_->findData(representation);
  if (index >= 0)
    selector_->setCurrentIndex(index);
}

QString CodeView::representation() const {
  return selector_->currentData().toString();
}

void CodeView::updateStatus() { status_->setText(text_->status()); }

bool CodeView::locked() const { return lock_->isChecked(); }

} // namespace neverd::gui
