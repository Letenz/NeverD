//===- DesktopBridge.h - Desktop bridge syntax -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fixed Tauri 1 / Wails 2 syntax shapes with exact lexical/source anchors.
/// This does not authenticate a runtime bridge or resolve a native callee.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Limits.h"
#include "neverd/web/SourceNavigation.h"

#include <array>

namespace neverd::web {
inline constexpr std::string_view DesktopBridgeProfile =
    "tauri-1-wails-2-direct-bridge-syntax-v1";

struct DesktopBridgeCall {
  std::string ID, Family, Form, Status, Reason, RootResolution;
  std::string CommandStatus;
  uint32_t Node = NoSourceIndex, Call = NoSourceIndex;
  uint32_t Root = NoSourceIndex, RootReference = NoSourceIndex;
  uint32_t Command = NoSourceIndex, Payload = NoSourceIndex;
  uint32_t Callback = NoSourceIndex, Error = NoSourceIndex;
  std::array<uint32_t, 3> TargetNames{NoSourceIndex, NoSourceIndex,
                                      NoSourceIndex};
  uint32_t Arguments = 0;
  bool Optional = false, Spread = false;
};
struct DesktopBridgeSource {
  std::string ID, SourceID, NavigationID, Status, Reason;
  std::vector<DesktopBridgeCall> Calls;
  uint64_t Steps = 0;
};
/// Only complete static window member chains and their direct argument
/// syntax. Computed/aliased/imported calls remain outside this first grammar.
/// Literal commands and target names are retained privately, never published.
DesktopBridgeSource
analyzeDesktopBridgeSource(const SourceAnalysis &Source,
                           const SourceBindingAnalysis &Bindings,
                           const SourceNavigation &Navigation);
} // namespace neverd::web
