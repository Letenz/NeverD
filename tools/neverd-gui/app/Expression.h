#pragma once

#include "Address.h"

#include <QCoreApplication>
#include <QHash>
#include <QString>
#include <QStringList>
#include <optional>

namespace neverd::gui {

/// Integer expressions as typed in the jump box and command line: numbers in
/// hexadecimal (0x10, 10h) or decimal (#10, 10 with a trailing dot), names,
/// and C operators with C precedence.  Plain digit runs are hexadecimal, as
/// in a disassembler's address prompts.
class Expression {
  Q_DECLARE_TR_FUNCTIONS(neverd::gui::Expression)

public:
  explicit Expression(QString text);

  /// Identifiers whose values the caller must supply before evaluate().
  QStringList identifiers() const;
  void bind(const QString &name, Address value) { names_.insert(name, value); }
  /// Evaluate with 64-bit wrapping arithmetic; nullopt with error() set on a
  /// syntax error, division by zero or an unbound name.
  std::optional<Address> evaluate();
  QString error() const { return error_; }

private:
  struct Token {
    enum Kind { Number, Name, Operator, Open, Close, End } kind = End;
    QString text;
    Address value = 0;
  };
  bool tokenize();
  std::optional<Address> parse(int precedence);
  std::optional<Address> unary();
  const Token &peek() const { return tokens_[position_]; }

  QString text_, error_;
  QVector<Token> tokens_;
  int position_ = 0;
  QHash<QString, Address> names_;
  bool tokenized_ = false, tokensValid_ = false;
};

} // namespace neverd::gui
