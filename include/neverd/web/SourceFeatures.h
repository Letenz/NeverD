//===- SourceFeatures.h - Client source syntax navigation --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fixed client syntax patterns and source-only callback relationships.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Limits.h"
#include "neverd/web/SourceBindings.h"

namespace neverd::web {

inline constexpr std::string_view JavaScriptClientFeatureProfile =
    "javascript-client-syntax-features-v1";

enum class SourceFeatureDomain { Tool, Configuration, Permission, MCP, Hook };
enum class SourceFeaturePattern {
  ToolDefinition,
  ToolRegistrationCall,
  ToolDispatchCall,
  ConfigurationCall,
  ConfigurationSection,
  PermissionCall,
  PermissionSection,
  MCPCall,
  MCPSection,
  HookCall,
  HookEvent,
  HookEventArgument
};

struct SourceFeature {
  std::string ID, Basis, Status, Reason, NameStatus;
  SourceFeatureDomain Domain = SourceFeatureDomain::Tool;
  SourceFeaturePattern Pattern = SourceFeaturePattern::ToolDefinition;
  uint32_t Node = NoSourceIndex, Trigger = NoSourceIndex;
  uint32_t Function = NoSourceIndex, Call = NoSourceIndex;
  uint32_t Name = NoSourceIndex, Callback = NoSourceIndex;
  uint32_t CallbackBinding = NoSourceIndex, CallbackFunction = NoSourceIndex;
  uint32_t Condition = NoSourceIndex;
  uint32_t Arguments = 0;
  uint32_t ArgumentOrdinal = NoSourceIndex;
  bool SpreadArguments = false;
};

struct SourceFeatures {
  std::string ID, SourceID, NavigationID, Status = "not_analyzed";
  std::vector<SourceFeature> Records;
  std::vector<SourceDiagnostic> Diagnostics;
  uint64_t Steps = 0, ComputedMemberCalls = 0;
};

struct SourceNavigation;
/// Spelling and object-shape candidates, not framework authentication or
/// effective policy. Callback links describe syntax/lexical initializers only.
/// Computed keys are never evaluated and no source value is published.
SourceFeatures
analyzeClientSourceFeatures(const SourceAnalysis &Source,
                            const SourceBindingAnalysis &Bindings,
                            const SourceNavigation &Navigation);

} // namespace neverd::web
