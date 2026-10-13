//===- ProtocolMethods.cpp - Fixed MCP method vocabulary ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// One closed method vocabulary shared by source and observed evidence.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/ProtocolMethods.h"

namespace neverd::web {
std::string classifyMCPMethod(std::string_view Name) {
  for (const auto Known : {"initialize",
                           "ping",
                           "tools/list",
                           "tools/call",
                           "resources/list",
                           "resources/templates/list",
                           "resources/read",
                           "resources/subscribe",
                           "resources/unsubscribe",
                           "prompts/list",
                           "prompts/get",
                           "completion/complete",
                           "logging/setLevel",
                           "sampling/createMessage",
                           "roots/list",
                           "elicitation/create",
                           "notifications/initialized",
                           "notifications/cancelled",
                           "notifications/progress",
                           "notifications/resources/updated",
                           "notifications/resources/list_changed",
                           "notifications/prompts/list_changed",
                           "notifications/tools/list_changed",
                           "notifications/message",
                           "notifications/roots/list_changed"})
    if (Name == Known)
      return Known;
  return "other_redacted";
}
} // namespace neverd::web
