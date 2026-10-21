//===- ConfigurationOrder.h - Configuration source order -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Ordered object-spread terms and lexical initializer source relationships.
/// No runtime configuration values or effective policy are inferred.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Limits.h"
#include "neverd/web/SourceFeatures.h"

#include <array>

namespace neverd::web {
inline constexpr std::string_view ConfigurationOrderProfile =
    "javascript-configuration-object-order-v1";

enum class ConfigurationField {
  Permissions,
  PermissionMode,
  MCPServers,
  Hooks
};
struct ConfigurationTerm {
  std::string ID, Kind;
  uint32_t Node = NoSourceIndex, Value = NoSourceIndex;
  uint32_t Reference = NoSourceIndex, Initializer = NoSourceIndex;
  uint32_t Feature = NoSourceIndex;
  bool UnknownKeys = false;
};
struct ConfigurationFieldOrder {
  bool Seen = false, LaterUnknown = false;
  uint32_t LastDefinition = NoSourceIndex;
  uint32_t DefinitionCount = 0;
};
struct ConfigurationObjectOrder {
  std::string ID;
  uint32_t Node = NoSourceIndex;
  std::vector<ConfigurationTerm> Terms;
  std::array<ConfigurationFieldOrder, 4> Fields;
};
struct ConfigurationSourceOrder {
  std::string ID, SourceID, NavigationID, FeatureID, Status, Reason;
  std::vector<ConfigurationObjectOrder> Objects;
  uint64_t Steps = 0, Terms = 0;
};
/// Only objects containing an admitted client settings-property candidate.
/// Unknown spreads/computed keys retain explicit possible overwrite barriers.
ConfigurationSourceOrder analyzeConfigurationSourceOrder(
    const SourceAnalysis &Source, const SourceBindingAnalysis &Bindings,
    const SourceNavigation &Navigation, const SourceFeatures &Features);
} // namespace neverd::web
