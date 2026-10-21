//===- ConfigurationFiles.h - Configuration syntax links -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fixed file/layer syntax sites and lexical source links through minified
/// names.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Limits.h"
#include "neverd/web/SourceNavigation.h"

namespace neverd::web {
inline constexpr std::string_view ConfigurationFilesProfile =
    "javascript-configuration-file-navigation-v1";

struct ConfigurationFileSite {
  std::string ID, Kind, Role;
  uint32_t Node = NoSourceIndex, Function = NoSourceIndex;
  uint32_t Call = NoSourceIndex, Case = NoSourceIndex;
};
struct ConfigurationFileCall {
  std::string ID;
  uint32_t Call = NoSourceIndex, TargetFunction = NoSourceIndex;
};
struct ConfigurationLayerTerm {
  uint32_t Node = NoSourceIndex, Ordinal = 0;
  std::string Role;
};
struct ConfigurationLayerSequence {
  std::string ID;
  uint32_t Node = NoSourceIndex, Function = NoSourceIndex;
  std::vector<ConfigurationLayerTerm> Terms;
};
struct ConfigurationFileNavigation {
  std::string ID, SourceID, BindingID, NavigationID, Status, Reason;
  std::vector<ConfigurationFileSite> Sites;
  std::vector<ConfigurationFileCall> Calls;
  std::vector<ConfigurationLayerSequence> Sequences;
  uint64_t Steps = 0, Terms = 0;
};

/// Direct literal vocabulary and local lexical relationships only. Array order
/// is source order; imports, alias chains, effective layers and values stay
/// unknown.
ConfigurationFileNavigation
analyzeConfigurationFiles(const SourceAnalysis &Source,
                          const SourceBindingAnalysis &Bindings,
                          const SourceNavigation &Navigation);
} // namespace neverd::web
