//===- Publication.h - Desktop bridge metadata -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Closed profile vocabulary and source anchors without commands or data.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/DesktopBridge.h"

#include "llvm/Support/JSON.h"

namespace neverd::web::desktop_bridge {
llvm::json::Object summary(const DesktopBridgeSource &Bridge);
llvm::json::Object record(const DesktopBridgeCall &Call,
                          const SourceAnalysis &Source,
                          const SourceBindingAnalysis &Bindings,
                          const SourceNavigation &Navigation);
} // namespace neverd::web::desktop_bridge
