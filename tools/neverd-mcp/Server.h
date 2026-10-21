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
#include "Tools.h"

#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::binmcp {
inline constexpr std::string_view ProtocolVersion = "2025-11-25";
/// The largest text a tool returns inline.
inline constexpr std::size_t MaxResult = 512 * 1024;
/// The snapshot text kept at once, and the largest result.
inline constexpr std::size_t MaxCachedResults = 8 * 1024 * 1024;
inline constexpr std::size_t MaxSnapshots = 4;

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
