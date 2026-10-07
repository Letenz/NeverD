#pragma once

#include "Address.h"

#include <QString>
#include <functional>
#include <optional>

class QObject;

namespace neverd::gui {

class Session;

/// Evaluate an address expression, resolving names through the worker.
/// `.` and `here` stand for \p here.  Unknown words made of hexadecimal digits
/// are numbers.  \p done receives the value or an error message.
void resolveExpression(
    Session &session, QObject *owner, const QString &text,
    std::optional<Address> here,
    std::function<void(std::optional<Address>, const QString &)> done);

} // namespace neverd::gui
