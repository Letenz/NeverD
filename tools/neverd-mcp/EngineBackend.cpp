//===- EngineBackend.cpp - The engine in this process ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The worker's engine, run in the MCP server's own process: one binary, and
/// no process boundary or framing between a query and its answer.
///
//===----------------------------------------------------------------------===//

#include "Backend.h"
#include "Engine.h"

#include <stdexcept>

namespace neverd::binmcp {
namespace {
std::string utf8(const std::filesystem::path &Path) {
  const auto Text = Path.u8string();
  return std::string(Text.begin(), Text.end());
}
} // namespace

EngineBackend::EngineBackend(const std::filesystem::path &Input)
    : Engine(std::make_unique<worker::Engine>()) {
  std::error_code Error;
  if (!std::filesystem::is_regular_file(Input, Error))
    throw std::runtime_error("--file must name an existing local input file");
  // The worker records canonical paths, as the workbench opens them.
  auto Path = std::filesystem::canonical(Input, Error);
  if (Error)
    Path = std::filesystem::absolute(Input);
  try {
    // Never written back. Each function's analysis is prepared between
    // requests because this engine answers the decompiles itself; the
    // workbench skips that, as its analysis replicas cannot reuse it.
    (void)Engine->executeResponse(
        "open", {{"path", utf8(Path)}, {"read_only", true}, {"warm", true}});
  } catch (const std::exception &Failure) {
    throw std::runtime_error(std::string("Could not open input: ") +
                             Failure.what());
  }
}

EngineBackend::~EngineBackend() = default;

Json EngineBackend::query(const std::string &Operation, const Json &Payload,
                          const std::optional<std::string> &ExpectedRevision) {
  // The worker's response, as its executor builds one.
  Json Response{
      {"protocol_major", 1}, {"type", "response"}, {"operation", Operation}};
  try {
    if (ExpectedRevision && *ExpectedRevision != Engine->revision())
      throw transport::Error(
          "stale_revision",
          "Project revision changed; refresh before retrying");
    auto Reply = Engine->executeResponse(Operation, Payload);
    // A cached page comes encoded, as the worker frames it.
    Response["payload"] = Reply.encoded
                              ? transport::parseJson(Reply.encoded->text(),
                                                     worker::MaxBackendBytes)
                              : std::move(Reply.value);
    Response["status"] = "ok";
  } catch (const transport::Error &Failure) {
    Response["status"] =
        Failure.code == "budget_exceeded" ? "budget_exceeded" : "error";
    Response["payload"] = Failure.detail;
    Response["error"] = {{"code", Failure.code}, {"message", Failure.what()}};
  } catch (const std::exception &Failure) {
    Response["status"] = "error";
    Response["payload"] = Json::object();
    Response["error"] = {{"code", "engine_error"}, {"message", Failure.what()}};
  }
  Response["revision"] = Engine->revision();
  Response["project_id"] = Engine->projectId();
  Response["analysis_state"] = Engine->analyzed() ? "complete" : "not_analyzed";
  return Response;
}

bool EngineBackend::hasIdleWork() const { return Engine->hasIdleWork(); }

bool EngineBackend::idleStep() {
  try {
    return Engine->idleStep();
  } catch (const std::exception &) {
    // A failed step leaves its error in the engine's background state, and
    // the request that needs its result reports it.
    return true;
  }
}

} // namespace neverd::binmcp
