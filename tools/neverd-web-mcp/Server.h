//===- Server.h - Offline web MCP lifecycle ---------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// JSON-RPC lifecycle separated from tool dispatch and byte framing.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "ResourceLimits.h"
#include "common/transport/Json.h"

#include <optional>

namespace neverd::mcp {
using transport::Json;
inline constexpr std::string_view ProtocolVersion = "2025-06-18";

/// Messages are fixed literals; never put request or target text here.
struct RpcError {
  int Code;
  const char *Message;
};

class ToolService {
public:
  virtual ~ToolService() = default;
  virtual Json list(const Json &Parameters) = 0;
  virtual Json call(const std::string &Name, const Json &Arguments) = 0;
};

Json errorReply(const Json &ID, int Code, const char *Message);

class Server {
  enum class State { Uninitialized, AwaitInitialized, Ready };
  State Current = State::Uninitialized;
  ToolService &Tools;

  Json request(const std::string &Method, const Json &Parameters);

public:
  explicit Server(ToolService &Tools) : Tools(Tools) {}
  /// Notifications and unsolicited client responses produce no reply.
  std::optional<Json> handle(std::string_view Message);
};
} // namespace neverd::mcp

// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::mcp {
inline constexpr std::size_t MaxRequestBytes =
    ::neverd::limits::web_mcp::kServerMaxRequestBytes;
inline constexpr std::size_t MaxResponseBytes =
    ::neverd::limits::web_mcp::kServerMaxResponseBytes;
inline constexpr std::size_t MaxToolJsonBytes =
    ::neverd::limits::web_mcp::kServerMaxToolJsonBytes;
} // namespace neverd::mcp
