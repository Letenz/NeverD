#pragma once
#include "Protocol.h"

#include "neverd/sdk/NeverDCAPISession.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace neverd::worker {
class ProjectLock;
class ProjectHistory;
class Contributions;
class GraphSnapshot;
class Listing;
class Engine {
public:
  Engine();
  ~Engine();
  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;
  Json execute(const std::string &operation, const Json &payload);
  using LoadProgressSink =
      std::function<void(const char *Phase, std::uint64_t Done,
                         std::uint64_t Total, const char *Detail)>;
  void setLoadProgressSink(LoadProgressSink Sink) {
    loadProgress_ = std::move(Sink);
  }
  std::string revision() const { return std::to_string(revision_); }
  std::string projectId() const { return projectId_; }
  bool analyzed() const { return analyzed_; }
  /// Background work (the reference index) runs between requests.
  bool hasIdleWork() const;
  void idleStep();
  /// {state, done, total, generation} for heartbeats; null before a load.
  Json backgroundState() const;
  static std::string version();

private:
  neverd_session_t session_ = nullptr;
  std::unique_ptr<ProjectLock> lock_;
  std::unique_ptr<ProjectHistory> history_;
  std::unique_ptr<Contributions> contributions_;
  std::unique_ptr<GraphSnapshot> graph_;
  std::string graphMetrics_;
  std::unique_ptr<Listing> listing_;
  // The function whose restricted pipeline the session currently holds.
  std::optional<std::uint64_t> preparedFunction_;
  std::uint64_t revision_ = 0;
  std::string projectId_;
  bool analyzed_ = false;
  bool dirty_ = false;
  bool readOnly_ = false;
  std::uintmax_t loadedSize_ = 0;
  std::filesystem::file_time_type loadedTime_;
  Json stringsCache_;
  std::string textKey_, textCache_;
  std::vector<std::size_t> textLines_;
  /// One whole code view under workbench names, paged from memory.
  std::string namedViewKey_;
  Json namedView_;
  std::vector<std::size_t> namedViewLines_;
  std::optional<Json> namedViewPage(std::uint64_t address,
                                    const std::string &representation,
                                    std::size_t offset, std::size_t limit);
  /// Function-list order (indices into Listing::functionRows) for the last
  /// filter, sort and listing generation.
  std::string functionOrderKey_;
  std::vector<std::size_t> functionOrder_;
  LoadProgressSink loadProgress_;
  void invalidate();
  void requireLoaded() const;
  void requireWriter() const;
  void analyze();
  /// Run the session's restricted single-function pipeline for \p address
  /// unless whole-image analysis already covers it.
  void prepareFunction(std::uint64_t address);
  Listing &listing();
  ProjectHistory &history();
  Json metadata() const;
  std::string error() const;
  Json backendJson(const char *owned, bool checkError = false) const;
};
} // namespace neverd::worker
