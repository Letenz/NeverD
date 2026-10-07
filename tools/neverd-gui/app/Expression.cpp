#include "Expression.h"

#include <QCoreApplication>

namespace neverd::gui {
namespace {
struct BinaryOperator {
  const char *spelling;
  int precedence;
};
// C precedence, loosest first.
constexpr BinaryOperator Operators[] = {
    {"|", 1}, {"^", 2}, {"&", 3}, {"<<", 4}, {">>", 4},
    {"+", 5}, {"-", 5}, {"*", 6}, {"/", 6},  {"%", 6},
};

int precedenceOf(const QString &spelling) {
  for (const auto &op : Operators)
    if (spelling == QLatin1String(op.spelling))
      return op.precedence;
  return 0;
}

} // namespace

Expression::Expression(QString text) : text_(std::move(text)) {}

bool Expression::tokenize() {
  if (tokenized_)
    return tokensValid_;
  tokenized_ = true;
  const QString &s = text_;
  int i = 0;
  while (i < s.size()) {
    const QChar c = s.at(i);
    if (c.isSpace()) {
      ++i;
      continue;
    }
    Token token;
    if (c == QLatin1Char('#') || c.isDigit()) {
      // `#` forces decimal; `0x` and a trailing `h` are hexadecimal; a
      // trailing `.` is decimal; a bare digit run is hexadecimal.
      const bool forcedDecimal = c == QLatin1Char('#');
      int start = forcedDecimal ? i + 1 : i;
      int end = start;
      while (end < s.size() &&
             (s.at(end).isLetterOrNumber() || s.at(end) == QLatin1Char('_')))
        ++end;
      QString digits = s.mid(start, end - start);
      bool decimal = forcedDecimal;
      if (!forcedDecimal && end < s.size() && s.at(end) == QLatin1Char('.')) {
        decimal = true;
        ++end;
      }
      bool ok = false;
      if (decimal) {
        token.value = digits.toULongLong(&ok, 10);
      } else {
        if (digits.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
          digits = digits.mid(2);
        else if (digits.endsWith(QLatin1Char('h'), Qt::CaseInsensitive))
          digits.chop(1);
        token.value = digits.toULongLong(&ok, 16);
      }
      if (!ok || digits.isEmpty()) {
        // Not a number after all (for example `1stPass`): a name.
        token.kind = Token::Name;
        token.text = s.mid(i, end - i);
      } else {
        token.kind = Token::Number;
      }
      i = end;
    } else if (c.isLetter() || c == QLatin1Char('_') || c == QLatin1Char('.') ||
               c == QLatin1Char('$') || c == QLatin1Char('?') ||
               c == QLatin1Char('@')) {
      int end = i + 1;
      while (end < s.size() &&
             (s.at(end).isLetterOrNumber() || s.at(end) == QLatin1Char('_') ||
              s.at(end) == QLatin1Char('.') || s.at(end) == QLatin1Char('$') ||
              s.at(end) == QLatin1Char('?') || s.at(end) == QLatin1Char('@') ||
              s.at(end) == QLatin1Char(':')))
        ++end;
      token.kind = Token::Name;
      token.text = s.mid(i, end - i);
      i = end;
    } else if (c == QLatin1Char('(')) {
      token.kind = Token::Open;
      ++i;
    } else if (c == QLatin1Char(')')) {
      token.kind = Token::Close;
      ++i;
    } else {
      const QString two = s.mid(i, 2);
      if (two == QLatin1String("<<") || two == QLatin1String(">>")) {
        token.text = two;
        i += 2;
      } else if (QStringLiteral("+-*/%&|^~").contains(c)) {
        token.text = QString(c);
        ++i;
      } else {
        error_ = tr("Unexpected character '%1'").arg(c);
        return false;
      }
      token.kind = Token::Operator;
    }
    tokens_.append(token);
  }
  tokens_.append(Token{});
  tokensValid_ = true;
  return true;
}

QStringList Expression::identifiers() const {
  auto *self = const_cast<Expression *>(this);
  QStringList result;
  if (!self->tokenize())
    return result;
  for (const auto &token : tokens_)
    if (token.kind == Token::Name && !result.contains(token.text))
      result.append(token.text);
  return result;
}

std::optional<Address> Expression::evaluate() {
  if (!tokenize())
    return std::nullopt;
  // Names may have been bound since a previous evaluation failed.
  error_.clear();
  position_ = 0;
  if (tokens_.size() <= 1) {
    error_ = tr("Empty expression");
    return std::nullopt;
  }
  auto value = parse(1);
  if (value && peek().kind != Token::End) {
    error_ = tr("Unexpected text after the expression");
    return std::nullopt;
  }
  return value;
}

std::optional<Address> Expression::unary() {
  const Token token = peek();
  ++position_;
  switch (token.kind) {
  case Token::Number:
    return token.value;
  case Token::Name:
    if (auto it = names_.constFind(token.text); it != names_.cend())
      return *it;
    error_ = tr("Unknown name '%1'").arg(token.text);
    return std::nullopt;
  case Token::Open: {
    auto value = parse(1);
    if (!value)
      return std::nullopt;
    if (peek().kind != Token::Close) {
      error_ = tr("Missing ')'");
      return std::nullopt;
    }
    ++position_;
    return value;
  }
  case Token::Operator:
    if (token.text == QLatin1String("-")) {
      auto value = unary();
      return value ? std::optional<Address>(~*value + 1) : std::nullopt;
    }
    if (token.text == QLatin1String("~")) {
      auto value = unary();
      return value ? std::optional<Address>(~*value) : std::nullopt;
    }
    if (token.text == QLatin1String("+"))
      return unary();
    break;
  default:
    break;
  }
  error_ = tr("Expected a value");
  return std::nullopt;
}

std::optional<Address> Expression::parse(int precedence) {
  auto left = unary();
  while (left && peek().kind == Token::Operator) {
    const int current = precedenceOf(peek().text);
    if (!current || current < precedence)
      break;
    const QString op = peek().text;
    ++position_;
    auto right = parse(current + 1);
    if (!right)
      return std::nullopt;
    const Address a = *left, b = *right;
    if ((op == QLatin1String("/") || op == QLatin1String("%")) && b == 0) {
      error_ = tr("Division by zero");
      return std::nullopt;
    }
    if (op == QLatin1String("+"))
      left = a + b;
    else if (op == QLatin1String("-"))
      left = a - b;
    else if (op == QLatin1String("*"))
      left = a * b;
    else if (op == QLatin1String("/"))
      left = a / b;
    else if (op == QLatin1String("%"))
      left = a % b;
    else if (op == QLatin1String("&"))
      left = a & b;
    else if (op == QLatin1String("|"))
      left = a | b;
    else if (op == QLatin1String("^"))
      left = a ^ b;
    else if (op == QLatin1String("<<"))
      left = b >= 64 ? 0 : a << b;
    else if (op == QLatin1String(">>"))
      left = b >= 64 ? 0 : a >> b;
  }
  return left;
}

} // namespace neverd::gui
