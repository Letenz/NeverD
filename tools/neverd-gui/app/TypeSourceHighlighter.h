#pragma once

#include "Theme.h"

#include <QRegularExpression>
#include <QSet>
#include <QSyntaxHighlighter>
#include <QTextDocument>

namespace neverd::gui {

/// Colors the C, C++, Rust or Go a local type renders as: keywords and
/// builtins, the program's types, numbers, directives and comments, the
/// offsets a comment gives standing out from its text.
class TypeSourceHighlighter final : public QSyntaxHighlighter {
public:
  explicit TypeSourceHighlighter(QTextDocument *document)
      : QSyntaxHighlighter(document) {}

  void setLanguage(const QString &language) {
    language_ = language;
    keywords_.clear();
    builtins_.clear();
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
    int comment = text.indexOf(QLatin1String("//"));
    if (comment < 0)
      comment = int(text.size());
    const QString code = text.left(comment);
    const QString trimmed = code.trimmed();
    if (trimmed.startsWith(QLatin1Char('#'))) {
      // A directive, or a Rust attribute.
      setFormat(0, comment, format(ColorRole::ListingDirective));
    } else {
      static const QRegularExpression token(QStringLiteral(
          R"((0x[0-9A-Fa-f]+|\b[0-9]+\b)|("[^"]*")|([A-Za-z_][A-Za-z0-9_]*))"));
      for (auto it = token.globalMatch(code); it.hasNext();) {
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
    if (comment < text.size()) {
      setFormat(comment, int(text.size()) - comment,
                format(ColorRole::ListingComment));
      // The offset and bits a comment gives are what a reader looks for.
      static const QRegularExpression where(QStringLiteral(
          R"(0x[0-9A-Fa-f]+|\+[0-9]+|\bbits? [0-9]+(\.\.[0-9]+)?)"));
      for (auto it = where.globalMatch(text, comment); it.hasNext();) {
        const auto match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(),
                  format(ColorRole::ListingNumber));
      }
    }
  }

private:
  QString language_;
  QSet<QString> keywords_, builtins_, names_;
};

} // namespace neverd::gui
