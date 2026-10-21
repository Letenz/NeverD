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

#ifndef NEVERD_TOOLS_NEVERD_MCP_RESOURCELIMITS_H
#define NEVERD_TOOLS_NEVERD_MCP_RESOURCELIMITS_H

#include "neverd/sdk/Limits.h"

#include <cstddef>
#include <cstdint>

namespace neverd::limits::mcp {
inline constexpr unsigned kMaxDecompilationLines =
    static_cast<unsigned>(::neverd::sdk::kMaxCodeViewPageLines);
inline constexpr unsigned kMaxDecompilationPages = 512;

// tools/neverd-mcp/Backend.cpp
/// Messages the workbench may have sent ahead of the one awaited.
inline constexpr std::size_t kBackendMaxQueued = 32;

// tools/neverd-mcp/Server.h
/// The largest text a tool returns inline.
inline constexpr std::size_t kServerMaxResult = 512 * 1024;
/// The snapshot text kept at once, and the largest result.
inline constexpr std::size_t kServerMaxCachedResults = 8 * 1024 * 1024;
inline constexpr std::size_t kServerMaxSnapshots = 4;

// tools/neverd-mcp/Stdio.h
/// The longest message a client may send.
inline constexpr std::size_t kStdioMaxLine = 1024 * 1024;
/// Requests that may wait at once.
inline constexpr std::size_t kStdioMaxPending = 16;

// tools/neverd-mcp/Tools.h
/// Strings a listing shows before it cuts them, unless the caller asks
/// otherwise.
inline constexpr unsigned kToolsDefaultStringTextLimit = 200;
/// Matches a search returns unless the caller asks otherwise.
inline constexpr unsigned kToolsDefaultSearchLimit = 64;

// tools/neverd-mcp/Backend.h
inline constexpr int kBackendAttachBackendSeconds = 120;

// tools/neverd-mcp/Main.cpp
inline constexpr int kMainSizeLimit = 32769;

// tools/neverd-mcp/Server.cpp
inline constexpr int kServerReadSnapshotCountLimit = 65536;

// tools/neverd-mcp/Stdio.cpp
inline constexpr int kStdioIdleGracePeriodMilliseconds = 150;
inline constexpr int kStdioServeMilliseconds = 10;

// tools/neverd-mcp/Tools.cpp
inline constexpr int kToolsAddressMaxLength = 512;
inline constexpr int kToolsPagePageLimitIntegerMaximum = 512;
inline constexpr int kToolsFilterMaxLength = 256;
inline constexpr int kToolsNodeMaxLength = 512;
inline constexpr int kToolsNodesMaxItems = 32;
inline constexpr int kToolsCatalogDepthIntegerMaximum = 64;
inline constexpr int kToolsCatalogReadDisassemblyLimitIntegerMaximum = 512;
inline constexpr int kToolsCatalogReadDecompilationLimitIntegerMaximum =
    ::neverd::sdk::kMaxCodeViewPageLines;
inline constexpr int kToolsCatalogReadBytesSizeIntegerMaximum = 4096;
inline constexpr int kToolsCatalogListStringsTextLimitIntegerMaximum = 1024;
inline constexpr int kToolsCatalogFindCallPathsMaxPathsIntegerMaximum = 200;
inline constexpr int kToolsCatalogSearchLimitIntegerMaximum = 512;

// tools/neverd-mcp/Backend.cpp
inline constexpr int kDefaultAttachTimeoutSeconds = 120;

// tools/neverd-mcp/Server.cpp
inline constexpr int kServerPageStepLimit = 64;
inline constexpr int kServerServerSearchStepLimit = 256;

} // namespace neverd::limits::mcp

#endif // NEVERD_TOOLS_NEVERD_MCP_RESOURCELIMITS_H
