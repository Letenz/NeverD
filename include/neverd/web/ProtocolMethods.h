//===- ProtocolMethods.h - Fixed MCP method classification --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared redaction vocabulary for explicitly selected MCP evidence.
///
//===----------------------------------------------------------------------===//

#pragma once
#include <string>
#include <string_view>

namespace neverd::web {
/// MCP 2025-06-18 known-method spelling; unknown input is never returned.
/// This classification validates neither an API nor a method-specific schema.
std::string classifyMCPMethod(std::string_view Name);
} // namespace neverd::web
