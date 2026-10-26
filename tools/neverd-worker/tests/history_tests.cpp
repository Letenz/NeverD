//===- history_tests.cpp - Durable history publication -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Sidecar preservation and recovery after interrupted transaction writes.
///
//===----------------------------------------------------------------------===//

#include "ProjectHistory.h"
#include "ResourceLimits.h"

#include "neverd/support/ProjectFiles.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace neverd::worker;
namespace fs = std::filesystem;
namespace files = neverd::project_files;

namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
std::string text(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  check(bool(input), "Cannot read test sidecar");
  return {std::istreambuf_iterator<char>(input), {}};
}
void writeFile(const fs::path &path, const std::string &value) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << value;
  output.close();
  check(bool(output), "Cannot write test sidecar");
}
struct Writer {
  std::vector<fs::path> calls;
  std::size_t failAt = std::numeric_limits<std::size_t>::max();
  bool failAfter = false;
  static inline Writer *active = nullptr;
  static void write(const fs::path &path, const std::string &value) {
    auto &self = *active;
    const bool fail = self.calls.size() == self.failAt;
    self.calls.push_back(path);
    if (fail && !self.failAfter)
      throw Error("injected_failure", "Before complete file replacement");
    writeFile(path, value);
    if (fail)
      throw Error("injected_failure", "After complete file replacement");
  }
  void clear() {
    calls.clear();
    failAt = std::numeric_limits<std::size_t>::max();
  }
};
struct Fixture {
  fs::path directory, binary;
  Writer writer;
  Fixture() {
    static unsigned sequence = 0;
    directory =
        fs::temp_directory_path() /
        ("neverd-history-unit-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(sequence++));
    binary = directory / "input.bin";
    writeFile(binary, "owned history fixture");
    Writer::active = &writer;
  }
  ~Fixture() {
    std::error_code ec;
    fs::remove_all(directory, ec);
    Writer::active = nullptr;
  }
  fs::path path(files::File id) const { return files::path(binary, id); }
  ProjectHistory open() const {
    return {binary, std::string(64, '0'), "fixture", false, Writer::write};
  }
};
Json annotation(const char *value) {
  return {{"addr", "0x1000"}, {"text", value}};
}
Json rename(const char *value) {
  return {{"addr", "0x1000"}, {"original", "function"}, {"renamed", value}};
}
Json initial(ProjectHistory &store) {
  auto state = store.committedState();
  state["annotations"] = Json::array({annotation("before")});
  state["renames"] = Json::array({rename("before")});
  store.persist(state);
  return state;
}
Json change(ProjectHistory &store, const char *value) {
  auto state = store.committedState();
  const auto beforeNote = state.at("annotations")[0].at("text");
  const auto beforeName = state.at("renames")[0];
  state["annotations"] = Json::array({annotation(value)});
  state["renames"] = Json::array({rename(value)});
  store.stage({{"kind", "annotation"},
               {"address", "0x1000"},
               {"before", beforeNote},
               {"after", value}});
  store.stage({{"kind", "rename"},
               {"address", "0x1000"},
               {"before", beforeName},
               {"after", state["renames"][0]}});
  return state;
}
template <typename F> void rejects(F &&action, const char *code) {
  try {
    action();
  } catch (const Error &error) {
    check(error.code == code, "Wrong transaction refusal");
    return;
  }
  check(false, "Invalid transaction accepted");
}
void unchangedFiles() {
  Fixture fixture;
  auto store = fixture.open();
  auto state = initial(store);
  const auto annotations = fixture.path(files::File::Annotations);
  const auto original = text(annotations);
  const auto timestamp = fs::last_write_time(annotations);
  fixture.writer.clear();
  state["renames"][0]["renamed"] = "after";
  store.persist(state);
  check(text(annotations) == original &&
            fs::last_write_time(annotations) == timestamp,
        "Unchanged sidecar was replaced");
  check(fixture.writer.calls ==
            std::vector<fs::path>{fixture.path(files::File::Journal),
                                  fixture.path(files::File::Renames),
                                  fixture.path(files::File::History)},
        "Commit wrote files outside its changed table and journal/history");
  check(text(fixture.path(files::File::Renames)) ==
            sidecarText(state["renames"]),
        "Changed sidecar lost canonical formatting");
  state["renames"] = Json::array();
  store.persist(state);
  check(Json::parse(text(fixture.path(files::File::Renames))).empty(),
        "Removing the final row did not persist an empty table");
  auto reopened = fixture.open();
  check(reopened.committedState() == state && reopened.current(),
        "Reopening changed the committed tables");

  // A foreign edit to an otherwise unchanged table must still block saving.
  writeFile(annotations, sidecarText(Json::array({annotation("foreign")})));
  fixture.writer.clear();
  rejects([&] { store.persist(state); }, "foreign_edits");
  check(fixture.writer.calls.empty() &&
            !ProjectHistory::recoveryPending(fixture.binary),
        "Conflict published transaction files");
}
void failureBoundaries() {
  // Two changed tables, plus the write-ahead journal and history document.
  constexpr std::size_t Writes = 4;
  for (std::size_t fail = 0; fail < Writes; ++fail)
    for (bool afterWrite : {false, true}) {
      Fixture fixture;
      auto store = fixture.open();
      const auto before = initial(store);
      const auto after = change(store, "after");
      fixture.writer.clear();
      fixture.writer.failAt = fail;
      fixture.writer.failAfter = afterWrite;
      rejects([&] { store.persist(after); }, "injected_failure");
      fixture.writer.clear();
      const bool published = fail != 0 || afterWrite;
      check(ProjectHistory::recoveryPending(fixture.binary) == published,
            "Write failure lost its recovery boundary");
      auto reopened = fixture.open();
      check(reopened.committedState() == (published ? after : before),
            "Recovery produced a mixed transaction state");
      check(!ProjectHistory::recoveryPending(fixture.binary) &&
                reopened.current(),
            "Recovered state still needs repair");
    }
}
void retryAndConflict() {
  for (bool foreignHistory : {false, true}) {
    Fixture fixture;
    auto store = fixture.open();
    initial(store);
    const auto after = change(store, "after");
    fixture.writer.clear();
    fixture.writer.failAt = 2;
    rejects([&] { store.persist(after); }, "injected_failure");
    fixture.writer.clear();
    const auto path = fixture.path(foreignHistory ? files::File::History
                                                  : files::File::Renames);
    const auto saved = text(path);
    writeFile(path, foreignHistory
                        ? "{}"
                        : sidecarText(Json::array({rename("foreign")})));
    const auto foreign = text(path);
    rejects([&] { fixture.open(); }, "foreign_edits");
    rejects([&] { store.persist(after); }, "foreign_edits");
    check(text(path) == foreign && fixture.writer.calls.empty(),
          "Recovery overwrote a foreign table or history");
    writeFile(path, saved);
    auto newer = after;
    newer["annotations"][0]["text"] = "newer";
    store.stage({{"kind", "annotation"},
                 {"address", "0x1000"},
                 {"before", "after"},
                 {"after", "newer"}});
    store.persist(newer);
    auto reopened = fixture.open();
    check(reopened.committedState() == newer &&
              !ProjectHistory::recoveryPending(fixture.binary),
          "Retry discarded newer staged state");
    check(reopened.next(false).at("after") == "newer",
          "Retry discarded newer history");
  }
}
void normalization() {
  auto state = ProjectHistory::normalizedState(
      {{"annotations", Json::array()}, {"renames", Json::array()}});
  state["annotations"] = Json::array({{{"addr", "0x20"}, {"text", "second"}},
                                      {{"addr", "0xA"}, {"text", "first"}}});
  auto sorted = ProjectHistory::normalizedState(state);
  check(sorted["annotations"][0]["addr"] == "0xa" &&
            sorted["annotations"][1]["addr"] == "0x20",
        "Unsorted address spellings did not normalize numerically");
  check(ProjectHistory::normalizedState(sorted) == sorted,
        "Already ordered state changed");
  state["annotations"].push_back({{"addr", "0xa"}, {"text", "duplicate"}});
  rejects([&] { ProjectHistory::normalizedState(state); }, "history_invalid");
}
void budgetBeforePublication() {
  Fixture fixture;
  auto store = fixture.open();
  const auto before = initial(store);
  const auto history = text(fixture.path(files::File::History));
  auto after = before;
  auto &rows = after["annotations"];
  rows = Json::array();
  constexpr std::size_t TextBytes = 1024;
  const auto count =
      neverd::limits::worker::kProjectHistoryMaxHistoryBytes / TextBytes + 1;
  for (std::size_t index = 0; index < count; ++index)
    rows.push_back(
        {{"addr", hexAddress(index)}, {"text", std::string(TextBytes, 'x')}});
  fixture.writer.clear();
  rejects([&] { store.persist(after); }, "history_budget_exceeded");
  check(fixture.writer.calls.empty() && store.committedState() == before &&
            text(fixture.path(files::File::History)) == history &&
            !ProjectHistory::recoveryPending(fixture.binary),
        "Oversized state published a partial transaction");
}
} // namespace
int main() {
  try {
    unchangedFiles();
    failureBoundaries();
    retryAndConflict();
    normalization();
    budgetBeforePublication();
    std::cout << "Unchanged sidecars, every write failure boundary, retries, "
                 "foreign edits and normalization passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
