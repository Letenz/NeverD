//===- Server.h - NeverD's read-only MCP server -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The MCP 2025-11-25 lifecycle, tools and resources over one backend.
///
/// A result larger than an inline text is kept as a snapshot resource that a
/// client reads in pages of characters, at the revision it was taken.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "Backend.h"
#include "ResourceLimits.h"
#include "Tools.h"

#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::binmcp {
inline constexpr std::string_view ProtocolVersion = "2025-11-25";

class Server {
public:
  explicit Server(Backend &Session);
  /// The result of one JSON-RPC message, null for one that has none.
  /// Throws RpcError.
  Json handle(const Json &Request);

  std::size_t snapshotCount() const { return Snapshots.size(); }
  std::size_t snapshotBytes() const { return CachedBytes; }

private:
  struct Snapshot {
    std::string URI, Text, MimeType, Encoding;
    Json Revision, ProjectID;
    std::size_t Characters = 0;
    /// The byte offset of every Stride-th character.
    std::vector<std::size_t> Marks;
    static constexpr std::size_t Stride = 4096;

    std::size_t byteAt(std::size_t Character) const;
  };

  Backend &Session;
  bool Initialized = false, Ready = false;
  std::vector<const Tool *> Tools;
  /// The oldest first.
  std::list<Snapshot> Snapshots;
  std::size_t CachedBytes = 0;
  std::uint64_t SnapshotSerial = 0;

  Json callTool(const Tool &Selected, Json Arguments);
  /// A search's page of matches, filled across an immediate search's
  /// decoding steps, with each code match's function and an immediate's
  /// instruction.
  Json search(const Json &Arguments);
  /// The whole text of the C function the decompilation page \p Response
  /// is: the page, when it runs from the first line to the last.
  static std::optional<std::string> pagedFunction(const Json &Response);
  /// The whole text of the C function a decompilation page \p Response,
  /// asked for with \p Arguments, is part of, read at the page's revision;
  /// none for another representation or when it cannot be read.
  std::optional<std::string> wholeFunction(const Json &Arguments,
                                           const Json &Response);
  /// The response that refused a name \p Arguments gives as its address, or
  /// nothing once the address is the one the name resolves to.
  std::optional<Json>
  resolveAddress(const std::string &Operation, Json &Arguments,
                 const std::optional<std::string> &Revision);
  /// The text inline, or a bounded snapshot link when it is too large to be.
  Json textResult(std::string Text, bool IsError, const Json &Response,
                  const char *MimeType = "text/plain",
                  const char *Encoding = "text");
  Json readResource(const Json &URI);
  Json readSnapshot(const std::string &URI, std::string_view Path,
                    std::string_view Query);
};

} // namespace neverd::binmcp

// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::binmcp {
inline constexpr std::size_t MaxResult =
    ::neverd::limits::mcp::kServerMaxResult;
inline constexpr std::size_t MaxCachedResults =
    ::neverd::limits::mcp::kServerMaxCachedResults;
inline constexpr std::size_t MaxSnapshots =
    ::neverd::limits::mcp::kServerMaxSnapshots;
} // namespace neverd::binmcp
