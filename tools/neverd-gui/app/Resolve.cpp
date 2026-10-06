#include "Resolve.h"

#include "Expression.h"
#include "Session.h"

#include <QJsonObject>
#include <memory>

namespace neverd::gui {

void resolveExpression(
    Session &session, QObject *owner, const QString &text,
    std::optional<Address> here,
    std::function<void(std::optional<Address>, const QString &)> done) {
  auto expression = std::make_shared<Expression>(text);
  QStringList names = expression->identifiers();
  for (const QString &alias : {QStringLiteral("."), QStringLiteral("here")})
    if (names.removeAll(alias) && here)
      expression->bind(alias, *here);
  if (names.isEmpty()) {
    const auto value = expression->evaluate();
    done(value, expression->error());
    return;
  }
  auto remaining = std::make_shared<int>(int(names.size()));
  auto finish =
      std::make_shared<std::function<void()>>([expression, remaining, done] {
        if (--*remaining == 0) {
          const auto value = expression->evaluate();
          done(value, expression->error());
        }
      });
  for (const QString &name : names)
    session.read(
        QStringLiteral("resolve"), {{"query", name}}, owner,
        [expression, name, finish](const QJsonObject &payload) {
          if (const auto address = addressValue(payload.value("address")))
            expression->bind(name, *address);
          (*finish)();
        },
        [expression, name, finish](const QString &, const QString &) {
          if (const auto value = parseAddress(name))
            expression->bind(name, *value);
          (*finish)();
        });
}

} // namespace neverd::gui
