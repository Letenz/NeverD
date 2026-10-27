//===- ResourceLimits.h - Resource limits and defaults -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resource ceilings, work budgets and defaults owned by this component.
/// Binary layouts and instruction/ABI constants remain with their models.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_WEB_MCP_RESOURCELIMITS_H
#define NEVERD_TOOLS_NEVERD_WEB_MCP_RESOURCELIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::web_mcp {

// tools/neverd-web-mcp/Catalog.cpp
inline constexpr std::size_t kCatalogTextMaximumBytes = 64;
inline constexpr std::size_t kCatalogPageLimit = 512;
inline constexpr std::size_t kCatalogPageSize = 16;

// tools/neverd-web-mcp/Catalog.h
inline constexpr std::size_t kCatalogMaximum = 64;
inline constexpr std::size_t kCatalogMinimum = 1;

// tools/neverd-web-mcp/Server.h
inline constexpr std::size_t kServerMaxRequestBytes = 64 * 1024;
inline constexpr std::size_t kServerMaxResponseBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kServerMaxToolJsonBytes = 2 * 1024 * 1024;

// tools/neverd-web-mcp/WebTools.h
inline constexpr std::size_t kWebToolsMaxInputs = 8;

// tools/neverd-web-mcp/Catalog.cpp
inline constexpr int kCatalogValidateValidBoundedStringArgument3 = 64;
inline constexpr int kCatalogToolDefinitionSchemaItemsMaxLength = 64;

// tools/neverd-web-mcp/Main.cpp
inline constexpr int kMainSizeLimit = 32769;

// tools/neverd-web-mcp/Server.cpp
inline constexpr int kServerValidIDBoundedStringArgument2 = 128;
inline constexpr int
    kServerRequestInitializeProtocolVersionBoundedStringArgument2 = 32;
inline constexpr int
    kServerRequestInitializeTitleVersionNameBoundedStringArgument2 = 128;
inline constexpr int
    kServerRequestInitializeTitleNameVersionBoundedStringArgument2 = 128;
inline constexpr int
    kServerRequestInitializeVersionNameTitleBoundedStringArgument2 = 256;
inline constexpr int kServerRequestCursorBoundedStringArgument2 = 128;
inline constexpr int kServerRequestArgumentsBoundedStringArgument2 = 128;
inline constexpr int kServerHandleMethodBoundedStringArgument2 = 128;

// tools/neverd-web-mcp/WebTools.cpp
inline constexpr int kWebToolsValidateInputsPathSizeLimit = 32768;

} // namespace neverd::limits::web_mcp

#endif // NEVERD_TOOLS_NEVERD_WEB_MCP_RESOURCELIMITS_H
