//===- Tools.h - The MCP server's tools and what they return ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The read-only tools a model calls, the checks of their arguments, and the
/// text a model reads for each worker response.
///
/// A tool returns what an agent reads: the worker's envelope, the
/// presentation data the workbench maps code with (rows, variables, call
/// sites, name spans) and repeated fields are left out, tables become columns
/// and rows, and code reads as text under a one-line JSON header.  Resources
/// keep the worker's complete JSON.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "ResourceLimits.h"
#include "common/transport/Json.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neverd::binmcp {
using transport::Json;

/// A JSON-RPC error a client reads.
struct RpcError {
  int Code;
  std::string Message;
};

struct Tool {
  /// The worker operation the tool runs.
  std::string Operation;
  /// What tools/list shows: name, description, inputSchema and annotations.
  Json Listed;

  const std::string &name() const {
    return Listed.at("name").get_ref<const std::string &>();
  }
  const Json &schema() const { return Listed.at("inputSchema"); }
};

/// The tools of every session.
const std::vector<Tool> &projectTools();
/// The tools of a workbench's shared session: its selection, and navigation.
const std::vector<Tool> &workbenchTools();

/// Strings a listing shows before it cuts them, unless the caller asks
/// otherwise.
/// Matches a search returns unless the caller asks otherwise.
/// The lines a decompilation page holds at most, as the worker bounds it,
/// and the pages read for one function's whole text.
inline constexpr unsigned MaxDecompilationLines =
    ::neverd::limits::mcp::kMaxDecompilationLines;
inline constexpr unsigned MaxDecompilationPages =
    ::neverd::limits::mcp::kMaxDecompilationPages;

/// Throws RpcError -32602 unless \p Arguments is an object that fits the
/// tool's input schema.
void validate(const Json &Arguments, const Json &Schema);

/// A member of an object; null when the value is no object or lacks it.
const Json &member(const Json &Object, const char *Key);
/// Whether a value counts as set: null, false, zero, and empty strings,
/// arrays and objects do not.
bool truthy(const Json &Value);

/// Whether \p Text is an exact hexadecimal address: 0x and 1-16 digits.
bool hexAddress(std::string_view Text);

/// The text an agent reads for one worker response, and whether it reports
/// an error.  \p Arguments are the tool's, for the shape they ask for.
std::pair<std::string, bool> compactResult(const std::string &Operation,
                                           const Json &Arguments,
                                           const Json &Response);

} // namespace neverd::binmcp

// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::binmcp {
inline constexpr unsigned DefaultStringTextLimit =
    ::neverd::limits::mcp::kToolsDefaultStringTextLimit;
inline constexpr unsigned DefaultSearchLimit =
    ::neverd::limits::mcp::kToolsDefaultSearchLimit;
} // namespace neverd::binmcp
