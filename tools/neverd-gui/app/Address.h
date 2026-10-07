#pragma once

#include <QJsonValue>
#include <QString>
#include <optional>

namespace neverd::gui {

using Address = quint64;

/// Worker protocol spelling: `0x` followed by lowercase hexadecimal.
inline QString hexAddress(Address value) {
  return QStringLiteral("0x") + QString::number(value, 16);
}

/// Display spelling: uppercase hexadecimal padded to \p digits.
inline QString displayAddress(Address value, int digits = 0) {
  return QStringLiteral("%1")
      .arg(value, digits, 16, QLatin1Char('0'))
      .toUpper();
}

/// Parse `0x1234`, `1234h` or bare hexadecimal digits.
inline std::optional<Address> parseAddress(QString text) {
  text = text.trimmed();
  if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
    text = text.mid(2);
  else if (text.endsWith(QLatin1Char('h'), Qt::CaseInsensitive))
    text.chop(1);
  if (text.isEmpty() || text.size() > 16)
    return std::nullopt;
  bool ok = false;
  const Address value = text.toULongLong(&ok, 16);
  return ok ? std::optional<Address>(value) : std::nullopt;
}

inline std::optional<Address> addressValue(const QJsonValue &value) {
  if (!value.isString())
    return std::nullopt;
  const QString text = value.toString();
  if (!text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
    return std::nullopt;
  return parseAddress(text);
}

} // namespace neverd::gui
