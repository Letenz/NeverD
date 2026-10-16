#pragma once

#include "Theme.h"

#include <QRegularExpression>
#include <QSet>
#include <QSyntaxHighlighter>
#include <QTextDocument>

namespace neverd::gui {

/// Colors the source a local type renders as: keywords and builtins, the
/// program's types, numbers, directives and comments, the offsets a comment
/// gives standing out from its text.
class TypeSourceHighlighter final : public QSyntaxHighlighter {
public:
  explicit TypeSourceHighlighter(QTextDocument *document)
      : QSyntaxHighlighter(document) {}

  void setLanguage(const QString &language) {
    language_ = language;
    keywords_.clear();
    builtins_.clear();
    // Python comments with #, which C and Rust start a directive or an
    // attribute with, and JavaScript a private field; Lua with --.
    const bool python = language == QLatin1String("python");
    const bool lua = language == QLatin1String("lua");
    commentMarker_ = python ? QStringLiteral("#")
                     : lua  ? QStringLiteral("--")
                            : QStringLiteral("//");
    directives_ = !python && !lua && language != QLatin1String("javascript");
    blockComments_ = language == QLatin1String("javascript");
    const QString family =
        language == QLatin1String("cpp") ? QStringLiteral("c") : language;
#define NEVERD_TYPE_SOURCE_KEYWORD(Language, Word)                             \
  if (family == QLatin1String(Language) ||                                     \
      language == QLatin1String(Language))                                     \
    keywords_.insert(QStringLiteral(Word));
#define NEVERD_TYPE_SOURCE_BUILTIN(Language, Word)                             \
  if (family == QLatin1String(Language))                                       \
    builtins_.insert(QStringLiteral(Word));
#include "TypeSourceVocabulary.def"
    rehighlight();
  }
  void setTypeNames(const QSet<QString> &names) {
    names_ = names;
    rehighlight();
  }
  /// The program's type \p word names: itself, or the type an anonymous
  /// record's generated name (_KTHREAD_0_1) belongs to.
  QString typeOf(QString word) const {
    static const QRegularExpression suffix(QStringLiteral("_[0-9]+$"));
    while (!word.isEmpty()) {
      if (names_.contains(word))
        return word;
      const auto match = suffix.match(word);
      if (!match.hasMatch())
        break;
      word.chop(match.capturedLength());
    }
    return {};
  }

protected:
  void highlightBlock(const QString &text) override {
    const auto &theme = Theme::instance();
    const auto format = [&](ColorRole role) {
      QTextCharFormat result;
      result.setForeground(theme.color(role));
      return result;
    };
    if (blockComments_) {
      highlightBlockComments(text, format);
      return;
    }
    int comment = text.indexOf(commentMarker_);
    if (comment < 0)
      comment = int(text.size());
    if (directives_ &&
        text.left(comment).trimmed().startsWith(QLatin1Char('#')))
      // A directive, or a Rust attribute.
      setFormat(0, comment, format(ColorRole::ListingDirective));
    else
      highlightCode(text, 0, comment, format);
    if (comment < text.size())
      highlightComment(text, comment, int(text.size()), format);
  }

private:
  /// Colors \p text's code between \p begin and \p end.
  template <typename Format>
  void highlightCode(const QString &text, int begin, int end,
                     const Format &format) {
    // Not a raw string: moc, which reads this header where a test includes
    // it, loses its way in one that holds quotes.
    static const QRegularExpression token(
        QStringLiteral("(0x[0-9A-Fa-f]+n?|\\b[0-9]+n?\\b)|(\"[^\"]*\")|"
                       "([A-Za-z_][A-Za-z0-9_]*)"));
    for (auto it = token.globalMatch(text.left(end), begin); it.hasNext();) {
      const auto match = it.next();
      if (match.capturedLength(1))
        setFormat(match.capturedStart(), match.capturedLength(),
                  format(ColorRole::ListingNumber));
      else if (match.capturedLength(2))
        setFormat(match.capturedStart(), match.capturedLength(),
                  format(ColorRole::ListingString));
      else if (const QString word = match.captured(3);
               keywords_.contains(word) || builtins_.contains(word))
        setFormat(match.capturedStart(), match.capturedLength(),
                  format(ColorRole::ListingKeyword));
      else if (!typeOf(word).isEmpty())
        setFormat(match.capturedStart(), match.capturedLength(),
                  format(ColorRole::ListingLibraryName));
    }
  }
  /// Colors a comment of \p text between \p begin and \p end.
  template <typename Format>
  void highlightComment(const QString &text, int begin, int end,
                        const Format &format) {
    setFormat(begin, end - begin, format(ColorRole::ListingComment));
    // The offset and bits a comment gives are what a reader looks for.
    static const QRegularExpression where(QStringLiteral(
        R"(0x[0-9A-Fa-f]+|\+[0-9]+|\bbits? [0-9]+(\.\.[0-9]+)?)"));
    for (auto it = where.globalMatch(text.left(end), begin); it.hasNext();) {
      const auto match = it.next();
      setFormat(match.capturedStart(), match.capturedLength(),
                format(ColorRole::ListingNumber));
    }
  }
  /// Colors a line of JavaScript, whose /* */ comments may span lines and
  /// sit between code: the block state says a comment is open.
  template <typename Format>
  void highlightBlockComments(const QString &text, const Format &format) {
    int at = 0;
    bool open = previousBlockState() == 1;
    const int size = int(text.size());
    while (at < size) {
      if (open) {
        const int close = int(text.indexOf(QLatin1String("*/"), at));
        const int end = close < 0 ? size : close + 2;
        highlightComment(text, at, end, format);
        at = end;
        open = close < 0;
        continue;
      }
      const int block = int(text.indexOf(QLatin1String("/*"), at));
      const int line = int(text.indexOf(QLatin1String("//"), at));
      if (line >= 0 && (block < 0 || line < block)) {
        highlightCode(text, at, line, format);
        highlightComment(text, line, size, format);
        at = size;
        continue;
      }
      const int end = block < 0 ? size : block;
      highlightCode(text, at, end, format);
      at = end;
      open = block >= 0;
    }
    setCurrentBlockState(open ? 1 : 0);
  }

  QString language_;
  QString commentMarker_ = QStringLiteral("//");
  bool directives_ = true;
  /// Whether /* */ comments may span lines, as JavaScript's JSDoc does.
  bool blockComments_ = false;
  QSet<QString> keywords_, builtins_, names_;
};

} // namespace neverd::gui
