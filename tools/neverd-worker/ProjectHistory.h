#pragma once
#include "Protocol.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace neverd::worker {
/// Replaces a file with the given text in one atomic step.
using AtomicWriter = void (*)(const std::filesystem::path &,
                              const std::string &);

// Durable user-edit history above the public C ABI. No analysis objects or
// pointers enter the store; recovery only replaces known user sidecar files.
class ProjectHistory {
public:
  ProjectHistory(std::filesystem::path binary, std::string sourceHash,
                 std::string engineVersion, bool readOnly, AtomicWriter writer);
  static bool recoveryPending(const std::filesystem::path &binary);
  bool recovered() const { return recovered_; }
  const Json &committedState() const;
  Json listing(std::size_t offset, std::size_t limit) const;
  /// Navigation/logging uses a patch row's virtual address, not its file key.
  static std::uint64_t displayAddress(const Json &command);
  /// Record an edit: {"kind","address","before","after"}, the row of the
  /// kind's table at that address on either side (null where none), and for
  /// a table of rows, optional "rows" [{"address","before","after"}]: the
  /// other rows the same edit changes, undone and redone with it.
  void stage(Json command);
  Json next(bool redo) const;
  void advance(bool redo);
  void persist(Json state);
  /// Called after each persist commits the project's files.
  void onCommit(std::function<void()> hook) { committed_ = std::move(hook); }
  void reset();
  /// Whether the project's files hold what this history last committed and
  /// its commands describe them: false after git, another tool or a person
  /// changed the files.
  bool current() const;
  std::string blockedReason() const { return blocked_; }
  /// Refuses an edit while the history is blocked (blockedReason).
  void requireUsable() const;
  void verifyLoadedState(const Json &state);
  static Json normalizedState(Json state);
  /// Read and validate committed user tables without loading edit history or
  /// mutating the project. This does not replay a pending transaction.
  static Json readState(const std::filesystem::path &binary);
  /// The state table a history command kind changes (UserStateTables.def),
  /// or null for an unknown kind.
  static const char *tableOfKind(std::string_view kind);

private:
  std::filesystem::path binary_, historyPath_, journalPath_;
  std::string hash_, engineVersion_, blocked_;
  bool readOnly_, recovered_ = false;
  std::uintmax_t fileSize_ = 0;
  std::filesystem::file_time_type fileTime_;
  AtomicWriter writer_;
  std::function<void()> committed_;
  Json commands_ = Json::array();
  struct Snapshot;
  // Copies used for rollback share only immutable, validated committed data.
  std::shared_ptr<const Snapshot> snapshot_;
  /// A journal this live history published before a failed sidecar write.
  /// Only that exact transaction can be completed by a same-session retry.
  Json interruptedWrite_;
  std::size_t cursor_ = 0;
  Json diskState() const;
  std::shared_ptr<const Snapshot> readSnapshot(Json *document = nullptr) const;
  std::string document(std::string_view encodedState) const;
  void recover();
};
} // namespace neverd::worker
