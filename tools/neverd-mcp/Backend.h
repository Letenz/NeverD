//===- Backend.h - Where the MCP server's queries run -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The engine a query runs on: one opened in this process, or the session of
/// a workbench that shares it.  Either answers as the worker protocol does,
/// with {type:"response",status,revision,project_id,payload|error}.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "ResourceLimits.h"
#include "common/transport/Json.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace neverd::worker {
class Engine;
}

namespace neverd::binmcp {
using transport::Json;

class Backend {
public:
  virtual ~Backend() = default;
  /// One operation's response; \p ExpectedRevision refuses it as stale when
  /// the project has another revision.  Throws std::runtime_error when the
  /// session itself fails.
  virtual Json query(const std::string &Operation, const Json &Payload,
                     const std::optional<std::string> &ExpectedRevision) = 0;
  /// Whether the session is a workbench's, which offers its selection and
  /// navigation.
  virtual bool attached() const = 0;
  /// Analysis that runs while no request waits: function discovery and the
  /// reference index, as the worker runs them between requests.  Each step
  /// is bounded, so a request waits for one step at most; a step that could
  /// not advance yet returns false.
  virtual bool hasIdleWork() const { return false; }
  virtual bool idleStep() { return false; }
};

/// An engine of this process with one input open read-only.
class EngineBackend final : public Backend {
  std::unique_ptr<worker::Engine> Engine;

public:
  explicit EngineBackend(const std::filesystem::path &Input);
  ~EngineBackend() override;
  Json query(const std::string &Operation, const Json &Payload,
             const std::optional<std::string> &ExpectedRevision) override;
  bool attached() const override { return false; }
  bool hasIdleWork() const override;
  bool idleStep() override;
};

/// A workbench session the user shared, reached through the credentials it
/// wrote: a local socket, or a named pipe on Windows, and a token.
class AttachBackend final : public Backend {
  struct Channel;
  std::unique_ptr<Channel> Session;
  std::uint64_t Serial = 0;

  Json next();

public:
  explicit AttachBackend(
      const std::filesystem::path &Credentials,
      std::chrono::seconds Timeout = std::chrono::seconds(
          ::neverd::limits::mcp::kBackendAttachBackendSeconds));
  ~AttachBackend() override;
  Json query(const std::string &Operation, const Json &Payload,
             const std::optional<std::string> &ExpectedRevision) override;
  bool attached() const override { return true; }
};

/// Worker framing: a 4-byte big-endian length, then that many bytes of a
/// JSON object, at most 8 MiB.
std::string encodeFrame(const Json &Message);
/// The size a frame header announces; throws for zero or more than 8 MiB.
std::uint32_t frameLength(const unsigned char Header[4]);

} // namespace neverd::binmcp
