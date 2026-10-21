#pragma once

#include "Session.h"

namespace neverd::gui {
// Same-build test seam: delivers a worker message as the session's worker
// connection does. Tests only; the workbench never includes this header.
struct SessionTestAccess {
  static void receive(Session &S, const QJsonObject &Message) {
    S.receive(Message);
  }
};
} // namespace neverd::gui
