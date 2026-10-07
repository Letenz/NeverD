#include "Engine.h"

#include "Contributions.h"
#include "EngineSymbols.h"
#include "GraphSnapshot.h"
#include "Listing.h"
#include "ProjectHistory.h"
#include "TextFold.h"

#include "neverd/sdk/NeverDCAPIDisasm.h"
#include "neverd/sdk/NeverDCAPIPersist.h"
#include "neverd/sdk/NeverDCAPIQuery.h"
#include "neverd/sdk/NeverDCAPISigs.h"
#include "neverd/support/ProjectWriteLock.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string_view>
#include <system_error>
#include <unordered_map>
#ifdef _WIN32
// std::min, std::max and numeric_limits<>::max() must not meet the macros.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace neverd::worker {
namespace fs = std::filesystem;
namespace {
using IRViewFunction = const char *(*)(neverd_session_t, neverd_va_t,
                                       const char *, std::size_t, std::size_t);
using StringsExFunction = const char *(*)(neverd_session_t, const char *);
using StringEncodingsFunction = const char *(*)();
using DecodeTextFunction = const char *(*)(const unsigned char *, int,
                                           const char *);
// Additive C ABI capabilities for strings in more than ASCII.
StringsExFunction stringsExFunction() {
  static const auto function =
      engineSymbol<StringsExFunction>("neverd_strings_ex_json");
  return function;
}
/// The engine's string encodings, or an empty array from an older engine.
const Json &stringEncodings() {
  static const Json encodings = [] {
    const auto function =
        engineSymbol<StringEncodingsFunction>("neverd_string_encodings_json");
    if (!function)
      return Json::array();
    const char *raw = function();
    Json parsed = Json::parse(raw ? raw : "[]", nullptr, false);
    neverd_free_string(raw);
    return parsed.is_array() ? parsed : Json::array();
  }();
  return encodings;
}
IRViewFunction irViewFunction() {
  // Additive C ABI capability: an older matching engine can still run the GUI.
  static const auto function =
      engineSymbol<IRViewFunction>("neverd_ir_view_json");
  return function;
}
/// Engine code text under workbench function names: every identifier token
/// that is an engine name with a display alias, outside string and character
/// literals.  \p shift receives (old byte offset, delta) for each edit.
std::string
renameIdentifiers(const std::string &text,
                  const std::unordered_map<std::string, std::string> &aliases,
                  std::vector<std::pair<std::size_t, std::ptrdiff_t>> &shift) {
  const auto identifierStart = [](unsigned char c) {
    return std::isalpha(c) || c == '_';
  };
  const auto identifierPart = [](unsigned char c) {
    return std::isalnum(c) || c == '_';
  };
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == '"' || c == '\'') {
      std::size_t j = i + 1;
      while (j < text.size() && text[j] != static_cast<char>(c) &&
             text[j] != '\n')
        j += text[j] == '\\' ? 2 : 1;
      j = std::min(text.size(), j + 1);
      out.append(text, i, j - i);
      i = j;
      continue;
    }
    if (identifierStart(c) &&
        (i == 0 || !identifierPart(static_cast<unsigned char>(text[i - 1])))) {
      std::size_t j = i + 1;
      while (j < text.size() &&
             identifierPart(static_cast<unsigned char>(text[j])))
        ++j;
      const std::string_view word(text.data() + i, j - i);
      if (const auto it = aliases.find(std::string(word));
          it != aliases.end()) {
        out += it->second;
        shift.emplace_back(i, static_cast<std::ptrdiff_t>(it->second.size()) -
                                  static_cast<std::ptrdiff_t>(word.size()));
      } else {
        out.append(word);
      }
      i = j;
      continue;
    }
    out += static_cast<char>(c);
    ++i;
  }
  return out;
}

/// A byte offset of the original text in the renamed text.
std::size_t shiftedOffset(
    std::size_t offset,
    const std::vector<std::pair<std::size_t, std::ptrdiff_t>> &shift) {
  std::ptrdiff_t delta = 0;
  for (const auto &[at, change] : shift) {
    if (at >= offset)
      break;
    delta += change;
  }
  return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(offset) + delta);
}

fs::path utf8Path(const std::string &text) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()),
                                text.size()));
}
void canonicalizeAddresses(Json &value) {
  if (value.is_array()) {
    for (auto &item : value)
      canonicalizeAddresses(item);
  } else if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it) {
      const auto &key = it.key();
      if (key == "addresses" && it->is_array()) {
        for (auto &address : *it)
          if (address.is_string())
            address = hexAddress(parseAddress(address.get<std::string>()));
      } else if ((key == "addr" || key == "va" || key == "address" ||
                  key == "start" || key == "end" || key == "entry" ||
                  key == "iat_addr" || key == "from" || key == "to") &&
                 it->is_string()) {
        const auto &text = it->get_ref<const std::string &>();
        if (text.starts_with("0x") || text.starts_with("0X"))
          *it = hexAddress(parseAddress(text));
      } else
        canonicalizeAddresses(*it);
    }
  }
}
std::string ownedString(const char *value) {
  std::unique_ptr<const char, decltype(&neverd_free_string)> owned(
      value, neverd_free_string);
  if (!value)
    return {};
  const auto size = strnlen(value, MaxBackendBytes + 1);
  if (size > MaxBackendBytes)
    throw Error("budget_exceeded",
                "Engine result exceeds the 32 MiB adapter budget");
  return std::string(value, size);
}
/// \p text folded for comparisons that ignore case, in any script.
std::string folded(const std::string &text) { return foldText(text); }
constexpr std::size_t MaxFunctionRows = 1000000;
// Strings need this many display columns unless string_options says
// otherwise; a minimum is at most MaxStringMinLength
// (neverd::strings::MaxMinLength).
constexpr int DefaultStringMinLength = 4;
constexpr std::int64_t MaxStringMinLength = 1024;
/// Lines of one engine code page; longer functions keep engine names.
constexpr std::size_t MaxNamedViewLines = 2048;

/// A page of \p items; a filter keeps the rows where one of \p searchFields
/// contains it, ignoring the case of ASCII letters.
Json page(const Json &items, const Json &payload,
          std::initializer_list<const char *> searchFields = {}) {
  const auto offset =
      sizeField(payload, "offset", 0, std::numeric_limits<std::size_t>::max());
  const auto limit = sizeField(payload, "limit", 128, 512);
  if (!limit)
    throw Error("invalid_request", "limit must be at least 1");
  const auto filter = folded(stringField(payload, "filter"));
  Json selected = Json::array();
  std::size_t total = 0;
  for (const auto &item : items) {
    if (!filter.empty() &&
        std::none_of(searchFields.begin(), searchFields.end(),
                     [&](const char *field) {
                       const auto it = item.find(field);
                       return it != item.end() && it->is_string() &&
                              folded(it->get<std::string>()).find(filter) !=
                                  std::string::npos;
                     }))
      continue;
    if (total >= offset && selected.size() < limit)
      selected.push_back(item);
    ++total;
  }
  const bool complete = offset >= total || selected.size() >= total - offset;
  return {{"items", std::move(selected)},
          {"total", total},
          {"offset", offset},
          {"next_offset", complete ? Json(nullptr) : Json(offset + limit)},
          {"complete", complete}};
}

/// Sort table rows by {"sort": field, "descending": bool}.  Hex address
/// strings compare numerically; other strings case-insensitively.
void sortItems(Json &items, const Json &payload) {
  const auto field = stringField(payload, "sort", {}, 64);
  if (field.empty() || !items.is_array())
    return;
  const bool descending = payload.value("descending", false);
  const auto key =
      [&](const Json &item) -> std::pair<std::uint64_t, std::string> {
    const auto it = item.find(field);
    if (it == item.end())
      return {0, {}};
    if (it->is_number_unsigned())
      return {it->get<std::uint64_t>(), {}};
    if (it->is_number_integer())
      return {static_cast<std::uint64_t>(it->get<std::int64_t>()), {}};
    if (it->is_string()) {
      const auto &text = it->get_ref<const std::string &>();
      if (text.size() > 2 && text[0] == '0' &&
          (text[1] == 'x' || text[1] == 'X'))
        try {
          return {parseAddress(text), {}};
        } catch (const Error &) {
        }
      return {0, folded(text)};
    }
    return {0, {}};
  };
  std::vector<std::pair<std::pair<std::uint64_t, std::string>, Json>> keyed;
  keyed.reserve(items.size());
  for (auto &item : items)
    keyed.emplace_back(key(item), std::move(item));
  std::stable_sort(keyed.begin(), keyed.end(),
                   [&](const auto &a, const auto &b) {
                     return descending ? b.first < a.first : a.first < b.first;
                   });
  items = Json::array();
  for (auto &entry : keyed)
    items.push_back(std::move(entry.second));
}

// Replace one sidecar atomically. Each sidecar is a separate durable file;
// this is intentionally not advertised as a transaction spanning both files.
void atomicWrite(const fs::path &path, const Json &value) {
  static std::atomic<std::uint64_t> sequence{0};
  fs::path temporary = path;
  temporary +=
      ".tmp-" +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) +
      "-" + std::to_string(sequence++);
  // Persistence validates compact serialized byte budgets. Write that same
  // encoding, otherwise indentation can produce an unreadable saved history.
  const auto body = value.dump(-1, ' ', false, Json::error_handler_t::replace);
#ifdef _WIN32
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    throw Error("save_failed", "Cannot create sidecar temporary file");
  DWORD written = 0;
  const bool success =
      WriteFile(file, body.data(), static_cast<DWORD>(body.size()), &written,
                nullptr) &&
      written == body.size() && FlushFileBuffers(file);
  CloseHandle(file);
  if (!success ||
      !MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str());
    throw Error("save_failed", "Cannot atomically replace sidecar");
  }
#else
  int descriptor =
      ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0)
    throw Error("save_failed", "Cannot create sidecar temporary file");
  std::size_t offset = 0;
  bool success = true;
  while (offset < body.size()) {
    const auto count =
        ::write(descriptor, body.data() + offset, body.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      success = false;
      break;
    }
    offset += static_cast<std::size_t>(count);
  }
  if (::fsync(descriptor) != 0)
    success = false;
  if (::close(descriptor) != 0)
    success = false;
  if (!success || ::rename(temporary.c_str(), path.c_str()) != 0) {
    ::unlink(temporary.c_str());
    throw Error("save_failed", "Cannot atomically replace sidecar");
  }
  int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC);
  if (directory >= 0) {
    (void)::fsync(directory);
    ::close(directory);
  }
#endif
}
} // namespace

class ProjectLock {
public:
  explicit ProjectLock(const fs::path &binary) : guard_(binary) {
    if (!guard_)
      throw Error("project_locked",
                  "Cannot acquire the sidecar writer lock: " + guard_.error());
  }

private:
  neverd::ProjectWriteLock guard_;
};

Engine::Engine()
    : session_(neverd_session_create()),
      contributions_(std::make_unique<Contributions>()) {
  if (!session_)
    throw Error("engine_unavailable", "Cannot create NeverD session");
}
Engine::~Engine() { neverd_session_destroy(session_); }
std::string Engine::version() { return ownedString(neverd_version_number()); }
std::string Engine::error() const {
  return ownedString(neverd_last_error(session_));
}
void Engine::requireLoaded() const {
  if (!neverd_session_is_loaded(session_))
    throw Error("not_loaded", "Open a binary first");
}
void Engine::requireWriter() const {
  requireLoaded();
  if (readOnly_ || !lock_)
    throw Error("read_only", "This session is read-only");
}
void Engine::invalidate() {
  graph_.reset();
  if (listing_)
    listing_->invalidate();
  stringsCache_ = nullptr;
  textKey_.clear();
  textCache_.clear();
  textLines_.clear();
  functionOrderKey_.clear();
  functionOrder_.clear();
}
std::optional<Json> Engine::namedViewPage(std::uint64_t address,
                                          const std::string &representation,
                                          std::size_t offset,
                                          std::size_t limit) {
  const auto view = irViewFunction();
  if (!view)
    return std::nullopt;
  const auto &aliases = listing().functionAliases();
  if (aliases.empty())
    return std::nullopt;
  const auto key = hexAddress(address) + ":" + representation + ":" +
                   std::to_string(revision_) + ":" +
                   std::to_string(listing().generation());
  if (namedViewKey_ != key) {
    namedViewKey_.clear();
    auto full = backendJson(
        view(session_, address, representation.c_str(), 0, MaxNamedViewLines),
        true);
    // A function longer than one engine page keeps the engine's own pages.
    if (!full.is_object() || !full.contains("text") ||
        !full["text"].is_string() || !full.value("complete", false) ||
        full.value("offset", std::size_t{0}) != 0)
      return std::nullopt;
    std::vector<std::pair<std::size_t, std::ptrdiff_t>> shift;
    full["text"] =
        renameIdentifiers(full["text"].get<std::string>(), aliases, shift);
    const std::size_t base = full.value("byte_offset", std::size_t{0});
    if (auto regions = full.find("library_regions");
        regions != full.end() && regions->is_array())
      for (auto &region : *regions)
        if (auto spans = region.find("spans");
            spans != region.end() && spans->is_array())
          for (auto &span : *spans)
            for (const char *field : {"begin_byte", "end_byte"})
              if (span.contains(field) && span[field].is_number_unsigned() &&
                  span[field].get<std::size_t>() >= base)
                span[field] =
                    base +
                    shiftedOffset(span[field].get<std::size_t>() - base, shift);
    // Renames before the definition move where it begins, not its line.
    if (auto prelude = full.find("prelude");
        prelude != full.end() && prelude->is_object() &&
        prelude->contains("end_byte") &&
        (*prelude)["end_byte"].is_number_unsigned() &&
        (*prelude)["end_byte"].get<std::size_t>() >= base)
      (*prelude)["end_byte"] =
          base + shiftedOffset((*prelude)["end_byte"].get<std::size_t>() - base,
                               shift);
    namedViewLines_.clear();
    const auto &text = full["text"].get_ref<const std::string &>();
    namedViewLines_.push_back(0);
    for (std::size_t i = 0; i < text.size(); ++i)
      if (text[i] == '\n' && i + 1 < text.size())
        namedViewLines_.push_back(i + 1);
    namedView_ = std::move(full);
    namedViewKey_ = key;
  }
  const auto &text = namedView_["text"].get_ref<const std::string &>();
  const std::size_t total = text.empty() ? 0 : namedViewLines_.size();
  const auto start = std::min(offset, total);
  const auto end = start + std::min(limit, total - start);
  const auto startByte = start < namedViewLines_.size() && !text.empty()
                             ? namedViewLines_[start]
                             : text.size();
  const auto endByte =
      end < namedViewLines_.size() ? namedViewLines_[end] : text.size();
  Json page = namedView_;
  page["text"] = text.substr(startByte, endByte - startByte);
  Json rows = Json::array();
  for (const auto &row : namedView_.value("rows", Json::array())) {
    const auto line = row.value("line", std::size_t{0});
    if (line >= start && line < end)
      rows.push_back(row);
  }
  page["rows"] = std::move(rows);
  page["offset"] = start;
  page["byte_offset"] =
      namedView_.value("byte_offset", std::size_t{0}) + startByte;
  page["total_lines"] = total;
  page["complete"] = end == total;
  page["next_offset"] = end == total ? Json(nullptr) : Json(end);
  page["project_id"] = projectId_;
  page["revision"] = revision();
  return page;
}

void Engine::analyze() {
  if (analyzed_)
    return;
  // A restricted single-function pipeline must not stand in for the image.
  neverd_session_restrict_function(session_, 0);
  preparedFunction_.reset();
  if (!neverd_session_analyze(session_))
    throw Error("analysis_failed", error());
  analyzed_ = true;
  invalidate();
  if (listing_)
    listing_->loadSwitches();
  ++revision_;
}
void Engine::prepareFunction(std::uint64_t address) {
  if (analyzed_ || preparedFunction_ == address)
    return;
  // VM images analyze the whole program as one unit.
  const auto arch = folded(ownedString(neverd_session_arch_name(session_)));
  if (arch == "evm" || arch == "sbf") {
    analyze();
    return;
  }
  // neverd_decompile() restricts the session pipeline to this entry and
  // replaces a previous restriction; IR, CFG and LLVM views then read it.
  (void)ownedString(neverd_decompile(session_, address));
  preparedFunction_ = address;
  graph_.reset();
  textKey_.clear();
}
Listing &Engine::newListing() {
  listing_ = std::make_unique<Listing>(session_);
  listing_->setStringOptions(stringOptions_);
  if (analyzed_)
    listing_->loadSwitches();
  return *listing_;
}

Listing &Engine::listing() {
  requireLoaded();
  if (!listing_)
    newListing();
  return *listing_;
}
bool Engine::hasIdleWork() const {
  return listing_ && neverd_session_is_loaded(session_) &&
         listing_->hasIdleWork();
}
void Engine::idleStep() {
  if (hasIdleWork())
    listing_->idleStep();
}
Json Engine::backgroundState() const {
  if (!listing_)
    return nullptr;
  return listing_->indexState();
}
Json Engine::backendJson(const char *owned, bool checkError) const {
  const auto text = ownedString(owned);
  if (checkError) {
    auto diagnostic = error();
    if (!diagnostic.empty())
      throw Error(diagnostic.find("budget") != std::string::npos
                      ? "budget_exceeded"
                      : "engine_error",
                  diagnostic);
  }
  if (text.empty())
    throw Error("engine_error",
                error().empty() ? "Engine returned no result" : error());
  try {
    auto value = Json::parse(text);
    canonicalizeAddresses(value);
    return value;
  } catch (const Json::exception &) {
    throw Error("engine_error", "Engine returned malformed JSON");
  }
}
ProjectHistory &Engine::history() {
  const auto path = utf8Path(ownedString(neverd_session_file_path(session_)));
  if (fs::file_size(path) != loadedSize_ ||
      fs::last_write_time(path) != loadedTime_)
    throw Error(
        "input_changed",
        "Input changed while the Session was open; reopen it before editing");
  if (!history_) {
    const auto arch = folded(ownedString(neverd_session_arch_name(session_)));
    if (arch == "evm" || arch == "sbf")
      analyze();
    // Hashing lives behind the C ABI; the worker gains no LLVM linkage.
    const auto dashboard = backendJson(neverd_dashboard_json(session_));
    const auto hash = dashboard.value("hashes", Json::object())
                          .value("sha256", std::string());
    history_ = std::make_unique<ProjectHistory>(
        utf8Path(ownedString(neverd_session_file_path(session_))), hash,
        version(), readOnly_, atomicWrite);
    if (history_->recovered()) {
      if (neverd_annotations_load(session_) != 0 ||
          neverd_renames_load(session_) != 0)
        throw Error("recovery_required",
                    "Recovered files could not be reloaded into the Session");
      dirty_ = false;
      invalidate();
      ++revision_;
    }
    history_->verifyLoadedState(
        {{"annotations", backendJson(neverd_annotations_json(session_))},
         {"renames", backendJson(neverd_renames_json(session_))}});
  }
  return *history_;
}
Json Engine::metadata() const {
  requireLoaded();
  const auto arch = ownedString(neverd_session_arch_name(session_));
  const bool deferred =
      !analyzed_ && (folded(arch) == "evm" || folded(arch) == "sbf");
  return {{"path", ownedString(neverd_session_file_path(session_))},
          {"architecture", arch},
          {"format", ownedString(neverd_session_format_name(session_))},
          {"bitness", neverd_session_bitness(session_)},
          {"file_size", std::to_string(neverd_session_file_size(session_))},
          {"base_address", hexAddress(neverd_session_base_addr(session_))},
          {"entry_address", hexAddress(neverd_session_entry_addr(session_))},
          {"function_count",
           deferred ? Json(nullptr) : Json(neverd_func_count(session_))},
          {"segment_count", neverd_session_segment_count(session_)},
          {"section_count", neverd_session_section_count(session_)},
          {"import_count", neverd_session_import_count(session_)},
          {"export_count", neverd_session_export_count(session_)},
          {"symbol_count", neverd_session_symbol_count(session_)},
          {"analyzed", analyzed_},
          {"analysis_state", analyzed_ ? "complete" : "not_analyzed"},
          {"read_only", readOnly_},
          {"dirty", dirty_}};
}

Json Engine::execute(const std::string &operation, const Json &p) {
  if (operation == "string_encodings") {
    if (stringEncodings().empty())
      throw Error("unsupported", "The engine finds only ASCII strings");
    return {{"items", stringEncodings()}};
  }
  if (operation == "string_options") {
    if (p.contains("encodings") || p.contains("min_length") ||
        p.contains("preferred")) {
      Json options = Json::object();
      const auto knownEncoding = [](const Json &name) {
        const auto known =
            name.is_string()
                ? std::find_if(stringEncodings().begin(),
                               stringEncodings().end(),
                               [&](const Json &encoding) {
                                 return encoding.value("name", std::string()) ==
                                        name.get<std::string>();
                               })
                : stringEncodings().end();
        if (known == stringEncodings().end())
          throw Error(
              "unsupported_encoding",
              "Unknown string encoding: " +
                  (name.is_string() ? name.get<std::string>() : name.dump()));
        return *known;
      };
      if (p.contains("encodings")) {
        const auto &names = p["encodings"];
        if (!names.is_array() || names.empty() || names.size() > 64)
          throw Error("invalid_request",
                      "encodings must be a non-empty array of names");
        for (const auto &name : names)
          knownEncoding(name);
        options["encodings"] = names;
      }
      // The code page that reads a C string first; the engine searches it
      // whether or not the encodings name it.
      if (p.contains("preferred") && !p["preferred"].is_null()) {
        if (!knownEncoding(p["preferred"]).value("legacy", false))
          throw Error("invalid_request",
                      "preferred must name a legacy code page");
        options["preferred"] = p["preferred"];
      }
      if (p.contains("min_length")) {
        const auto &length = p["min_length"];
        if (!length.is_number_integer() || length.get<std::int64_t>() < 1 ||
            length.get<std::int64_t>() > MaxStringMinLength)
          throw Error("invalid_request",
                      "min_length must be an integer from 1 to " +
                          std::to_string(MaxStringMinLength));
        options["min_length"] = length;
      }
      stringOptions_ = options.dump();
      if (listing_)
        listing_->setStringOptions(stringOptions_);
      stringsCache_ = nullptr;
      ++revision_;
    }
    Json current =
        stringOptions_.empty() ? Json::object() : Json::parse(stringOptions_);
    if (!current.contains("encodings")) {
      current["encodings"] = Json::array();
      for (const auto &encoding : stringEncodings())
        if (encoding.value("default", false))
          current["encodings"].push_back(encoding.value("name", std::string()));
    }
    if (!current.contains("min_length"))
      current["min_length"] = DefaultStringMinLength;
    if (!current.contains("preferred"))
      current["preferred"] = nullptr;
    return current;
  }
  if (operation == "contributions")
    return contributions_->listing();
  if (operation == "contribution_register")
    return contributions_->registerFile(stringField(p, "path", {}, 32768));
  if (operation == "contribution_unregister")
    return contributions_->remove(stringField(p, "namespace", {}, 64));
  if (operation == "contribution_execute") {
    requireLoaded();
    for (auto it = p.begin(); it != p.end(); ++it)
      if (it.key() != "id" && it.key() != "address")
        throw Error("invalid_request",
                    "Contribution execution accepts only id and address");
    const auto id = stringField(p, "id", {}, 129);
    const auto address = stringField(
        p, "address", hexAddress(neverd_session_entry_addr(session_)), 18);
    (void)parseAddress(address);
    const auto query = contributions_->query(id, address);
    return {{"contribution_id", id},
            {"operation", query.at("operation")},
            {"result", execute(query.at("operation").get<std::string>(),
                               query.at("payload"))}};
  }
  if (operation == "open") {
    const auto pathText = stringField(p, "path", {}, 32768);
    if (pathText.empty())
      throw Error("invalid_request", "path is required");
    std::error_code ec;
    const auto path = fs::canonical(utf8Path(pathText), ec);
    if (ec || !fs::is_regular_file(path, ec))
      throw Error("load_failed", "Input is not an accessible regular file");
    if (p.contains("read_only") && !p["read_only"].is_boolean())
      throw Error("invalid_request", "read_only must be boolean");
    const bool readOnly = p.value("read_only", false);
    auto currentPath =
        utf8Path(ownedString(neverd_session_file_path(session_)));
    const bool reuseLock = !readOnly && lock_ && currentPath == path;
    std::unique_ptr<ProjectLock> nextLock;
    if (!readOnly && !reuseLock)
      nextLock = std::make_unique<ProjectLock>(path);
    std::unique_ptr<void, decltype(&neverd_session_destroy)> next(
        neverd_session_create(), neverd_session_destroy);
    if (!next)
      throw Error("engine_unavailable", "Cannot create NeverD session");
    const auto utf8 = path.u8string();
    const std::string loadPath(utf8.begin(), utf8.end());
    const auto fileSize = fs::file_size(path);
    const auto fileTime = fs::last_write_time(path);
    if (loadProgress_)
      neverd_session_set_load_progress(
          next.get(),
          [](void *User, const char *Phase, unsigned long long Done,
             unsigned long long Total, const char *Detail) {
            auto *Sink = static_cast<LoadProgressSink *>(User);
            if (Sink && *Sink)
              (*Sink)(Phase, Done, Total, Detail);
          },
          &loadProgress_);
    if (!neverd_session_load(next.get(), loadPath.c_str()))
      throw Error("load_failed", ownedString(neverd_last_error(next.get())));
    if (fs::file_size(path) != fileSize ||
        fs::last_write_time(path) != fileTime)
      throw Error(
          "input_changed",
          "Input changed while loading; retry after the writer finishes");
    neverd_session_destroy(session_);
    session_ = next.release();
    if (!reuseLock)
      lock_ = std::move(nextLock);
    readOnly_ = readOnly;
    dirty_ = false;
    analyzed_ = false;
    loadedSize_ = fileSize;
    loadedTime_ = fileTime;
    history_.reset();
    preparedFunction_.reset();
    newListing();
    invalidate();
    ++revision_;
    projectId_ = "project-" + std::to_string(revision_);
    Json warnings = Json::array();
    if (ProjectHistory::recoveryPending(path)) {
      try {
        (void)history();
      } catch (const Error &error) {
        warnings.push_back(std::string(error.what()));
      }
    }
    if (neverd_annotations_load(session_) != 0)
      warnings.push_back("Annotations sidecar could not be loaded");
    if (neverd_renames_load(session_) != 0)
      warnings.push_back("Renames sidecar could not be loaded");
    auto result = metadata();
    result["warnings"] = warnings;
    return result;
  }
  requireLoaded();
  if (operation == "signatures_load") {
    const auto path = stringField(p, "path", {}, 32768);
    const auto mode = stringField(p, "mode", "file", 16);
    if (path.empty() || (mode != "file" && mode != "auto"))
      throw Error("invalid_request",
                  "A signature path and file/auto mode are required");
    const int matches =
        mode == "auto" ? neverd_auto_apply_signatures(session_, path.c_str())
                       : neverd_apply_signature_file(session_, path.c_str());
    if (matches < 0)
      throw Error("signature_load_failed", error());
    // This changes analysis evidence only. It is also available on a read-only
    // image and never stages a sidecar edit or modifies the binary.
    analyzed_ = false;
    invalidate();
    ++revision_;
    return {{"loaded", true}, {"byte_matches", matches}};
  }
  if (operation == "metadata")
    return metadata();
  if (operation == "history") {
    const auto limit = sizeField(p, "limit", 128, 512);
    if (!limit)
      throw Error("invalid_request", "limit must be at least 1");
    return history().listing(
        sizeField(p, "offset", 0, std::numeric_limits<std::size_t>::max()),
        limit);
  }
  if (operation == "history_reset") {
    requireWriter();
    if (dirty_)
      throw Error("unsaved_changes",
                  "Save or reload staged annotations before resetting history");
    history().reset();
    if (neverd_annotations_load(session_) != 0 ||
        neverd_renames_load(session_) != 0)
      throw Error("reload_failed",
                  "History was reset but user state could not be reloaded",
                  {{"saved", true}});
    invalidate();
    ++revision_;
    return history().listing(0, 128);
  }
  if (operation == "undo" || operation == "redo") {
    requireWriter();
    auto &store = history();
    const bool redo = operation == "redo";
    const auto command = store.next(redo);
    const auto value = command.at(redo ? "after" : "before");
    const auto address = parseAddress(command.at("address").get<std::string>());
    if (command.at("kind") == "annotation") {
      store.advance(redo);
      neverd_annotation_set(session_, address,
                            value.get_ref<const std::string &>().c_str());
      dirty_ = true;
    } else {
      if (dirty_)
        throw Error(
            "unsaved_changes",
            "Save or reload staged annotations before undoing a rename");
      auto state = store.committedState();
      auto &renames = state["renames"];
      renames.erase(
          std::remove_if(renames.begin(), renames.end(),
                         [&](const Json &row) {
                           return parseAddress(
                                      row.at("addr").get<std::string>()) ==
                                  address;
                         }),
          renames.end());
      if (!value.is_null())
        renames.push_back(value);
      const auto checkpoint = store;
      store.advance(redo);
      try {
        store.persist(state);
      } catch (...) {
        store = checkpoint;
        throw;
      }
      if (neverd_renames_load(session_) != 0)
        throw Error("reload_failed",
                    "History operation was saved but cannot be reloaded",
                    {{"saved", true}});
    }
    invalidate();
    ++revision_;
    auto result = store.listing(0, 128);
    result["dirty"] = dirty_;
    result["saved"] = !dirty_;
    result["address"] = hexAddress(address);
    return result;
  }
  if (operation == "analyze") {
    analyze();
    return metadata();
  }
  if (operation == "functions" || operation == "resolve" ||
      operation == "disasm") {
    const auto arch = folded(ownedString(neverd_session_arch_name(session_)));
    // Those C ABI paths trigger the entire pipeline for VM architectures.
    // Publish its completion and revision explicitly rather than silently
    // mutating the Session under a revision advertised as not analyzed.
    if (arch == "evm" || arch == "sbf")
      analyze();
  }
  if (operation == "functions") {
    const auto offset =
        sizeField(p, "offset", 0, std::numeric_limits<std::size_t>::max());
    const auto limit = sizeField(p, "limit", 128, 512);
    if (!limit)
      throw Error("invalid_request", "limit must be at least 1");
    const auto filter = folded(stringField(p, "filter"));
    const auto sort = stringField(p, "sort", {}, 64);
    const bool descending = p.value("descending", false);
    auto &listingView = listing();
    const auto &rows = listingView.functionRows();
    if (rows.size() > MaxFunctionRows)
      throw Error("budget_exceeded",
                  "The function index exceeds one million entries");
    std::string key = filter;
    key += '\0';
    key += sort;
    key += descending ? "\0d\0" : "\0a\0";
    key += std::to_string(listingView.generation());
    if (key != functionOrderKey_) {
      functionOrder_.clear();
      for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto &row = rows[i];
        // The filter matches the name, the engine's name, the name as it
        // reads demangled or the address.
        const auto matches = [&](const char *field) {
          return folded(row.value(field, std::string())).find(filter) !=
                 std::string::npos;
        };
        if (filter.empty() || matches("name") || matches("engine_name") ||
            matches("demangled_name") ||
            row.value("address", std::string()).find(filter) !=
                std::string::npos)
          functionOrder_.push_back(i);
      }
      if (!sort.empty()) {
        Json keyed = Json::array();
        for (const auto i : functionOrder_) {
          Json item = rows[i];
          item["\x01index"] = i;
          keyed.push_back(std::move(item));
        }
        sortItems(keyed, p);
        functionOrder_.clear();
        for (const auto &item : keyed)
          functionOrder_.push_back(item["\x01index"].get<std::size_t>());
      }
      functionOrderKey_ = std::move(key);
    }
    const auto total = functionOrder_.size();
    Json items = Json::array();
    for (auto i = std::min(offset, total); i < total && items.size() < limit;
         ++i) {
      const auto &row = rows[functionOrder_[i]];
      const auto address = parseAddress(row["address"].get<std::string>());
      // The engine's identity (linkage and demangled names) under the
      // workbench's display name.
      auto item = backendJson(neverd_resolve_addr(session_, address), false);
      if (!item.is_object())
        item = Json::object();
      const bool renamed = row.contains("engine_name");
      for (const auto &[field, value] : row.items())
        if (field != "demangled_name")
          item[field] = value;
      // A name the workbench gave reads demangled, as the engine's do.
      if (renamed || !item.contains("display_name"))
        item["display_name"] = row.value("demangled_name", row["name"]);
      items.push_back(std::move(item));
    }
    const bool complete = offset >= total || items.size() >= total - offset;
    return {{"items", items},
            {"offset", offset},
            {"total", total},
            {"complete", complete},
            {"next_offset",
             complete ? Json(nullptr) : Json(offset + items.size())}};
  }
  if (operation == "resolve") {
    const auto query = stringField(p, "query");
    if (query.empty())
      throw Error("invalid_request", "query is required");
    int index = -1;
    std::uint64_t address = 0;
    if (query.starts_with("0x") || query.starts_with("0X")) {
      address = parseAddress(query);
      index = neverd_func_find_by_addr(session_, address);
      if (index < 0) {
        const auto count = neverd_func_count(session_);
        for (int i = 0; i < count; ++i) {
          const auto entry = neverd_func_entry(session_, i);
          const int size = neverd_func_size(session_, i);
          if (entry <= address && size > 0 &&
              address - entry < static_cast<std::uint64_t>(size)) {
            index = i;
            break;
          }
        }
      }
    } else {
      index = neverd_func_find_by_name(session_, query.c_str());
      if (index >= 0) {
        address = neverd_func_entry(session_, index);
      } else if (auto named = listing().resolveName(query)) {
        address = *named;
        index = neverd_func_find_by_addr(session_, address);
      } else {
        throw Error("not_found", "No function or name matches that symbol");
      }
    }
    auto result =
        index < 0
            ? Json::object()
            : backendJson(neverd_resolve_addr(
                              session_, neverd_func_entry(session_, index)),
                          false);
    result["address"] = hexAddress(address);
    result["function_address"] =
        index < 0 ? Json(nullptr)
                  : Json(hexAddress(neverd_func_entry(session_, index)));
    std::string name = index < 0
                           ? std::string()
                           : ownedString(neverd_func_name(session_, index));
    // The workbench name of a function the engine leaves generic.
    if (const auto &aliases = listing().functionAliases();
        aliases.contains(name))
      name = aliases.at(name);
    result["name"] = std::move(name);
    result["comment"] = ownedString(neverd_annotation_get(session_, address));
    return result;
  }
  if (operation == "strings") {
    if (stringsCache_.is_null()) {
      const auto stringsEx = stringsExFunction();
      stringsCache_ = backendJson(
          stringsEx ? stringsEx(session_, stringOptions_.empty()
                                              ? nullptr
                                              : stringOptions_.c_str())
                    : neverd_strings_json(session_, DefaultStringMinLength));
      for (auto &item : stringsCache_) {
        item["address"] = item.at("addr");
        item["text"] = item.at("value");
        item.erase("addr");
        // The classic type column: C for plain bytes, else the encoding.
        const auto encoding = item.value("encoding", std::string("ascii"));
        std::string type = "C";
        for (const auto &known : stringEncodings())
          if (known.value("name", std::string()) == encoding &&
              !known.value("spelling", std::string()).empty())
            type = known.value("spelling", std::string());
        item["type"] = type;
      }
    }
    return page(stringsCache_, p, {"text", "address", "type"});
  }
  if (operation == "segments") {
    auto items = backendJson(neverd_segments_json(session_));
    for (auto &item : items) {
      item["address"] = item.at("va");
      item.erase("va");
    }
    return page(items, p, {"name"});
  }
  if (operation == "listing")
    return listing().page(p);
  if (operation == "string_references")
    return listing().stringReferences(p);
  if (operation == "overview")
    return listing().overview(p);
  if (operation == "names") {
    auto items = listing().names();
    sortItems(items, p);
    return page(items, p, {"name"});
  }
  if (operation == "regions") {
    auto items = listing().regions();
    sortItems(items, p);
    return page(items, p, {"name"});
  }
  if (operation == "imports") {
    Json items = Json::array();
    for (const auto &row : backendJson(neverd_imports_json(session_)))
      if (row.contains("iat_addr") &&
          parseAddress(row["iat_addr"].get<std::string>()))
        items.push_back({{"address", row["iat_addr"]},
                         {"name", row.value("name", std::string())},
                         {"module", row.value("module", std::string())},
                         {"ordinal", row.value("ordinal", 0)}});
    sortItems(items, p);
    return page(items, p, {"name"});
  }
  if (operation == "exports") {
    Json items = Json::array();
    for (const auto &row : backendJson(neverd_exports_json(session_)))
      items.push_back({{"address", row.value("addr", std::string("0x0"))},
                       {"name", row.value("name", std::string())},
                       {"ordinal", row.value("ordinal", 0)},
                       {"kind", "export"}});
    for (const auto &row : backendJson(neverd_entrypoints_json(session_)))
      items.push_back({{"address", row.value("addr", std::string("0x0"))},
                       {"name", row.value("name", std::string())},
                       {"ordinal", nullptr},
                       {"kind", row.value("type", std::string("entry"))}});
    sortItems(items, p);
    return page(items, p, {"name"});
  }
  if (operation == "search") {
    const auto kind = stringField(p, "kind", "text", 16);
    const auto pattern = stringField(p, "pattern", {}, 4096);
    const auto limit = sizeField(p, "limit", 256, 4096);
    if (pattern.empty() || !limit)
      throw Error("invalid_request", "A search pattern and limit are required");
    Json hits;
    if (kind == "bytes") {
      std::vector<unsigned char> bytes;
      std::string digits;
      for (char c : pattern)
        if (std::isxdigit(static_cast<unsigned char>(c)))
          digits += c;
        else if (c != ' ' && c != '\t')
          throw Error("invalid_request",
                      "Byte patterns contain hexadecimal pairs");
      if (digits.empty() || digits.size() % 2)
        throw Error("invalid_request",
                    "Byte patterns contain hexadecimal pairs");
      for (std::size_t i = 0; i < digits.size(); i += 2)
        bytes.push_back(static_cast<unsigned char>(
            std::stoul(digits.substr(i, 2), nullptr, 16)));
      hits = backendJson(neverd_search_bytes(session_, bytes.data(),
                                             static_cast<int>(bytes.size()),
                                             static_cast<int>(limit)));
    } else if (kind == "text") {
      hits = backendJson(neverd_search_string(
          session_, pattern.c_str(), p.value("case_sensitive", false) ? 1 : 0,
          static_cast<int>(limit)));
    } else {
      throw Error("invalid_request", "Search kind must be bytes or text");
    }
    Json items = Json::array();
    for (auto &hit : hits) {
      hit["address"] = hit.value("addr", std::string("0x0"));
      hit.erase("addr");
      items.push_back(std::move(hit));
    }
    return {{"items", std::move(items)}, {"complete", true}};
  }
  if (operation == "annotations") {
    auto items = backendJson(neverd_annotations_json(session_));
    for (auto &item : items) {
      item["address"] = item.at("addr");
      item.erase("addr");
    }
    return page(items, p, {"text"});
  }
  if (operation == "save") {
    requireWriter();
    auto &store = history();
    auto state = store.committedState();
    state["annotations"] = backendJson(neverd_annotations_json(session_));
    store.persist(state);
    dirty_ = false;
    return {{"saved", true}, {"dirty", false}};
  }
  if (operation == "reload") {
    const bool annotationsLoaded = neverd_annotations_load(session_) == 0;
    const bool renamesLoaded = neverd_renames_load(session_) == 0;
    // Each existing API can change its own table independently. Publish a new
    // revision even on partial failure so clients cannot retain stale pages.
    invalidate();
    ++revision_;
    history_.reset();
    if (!annotationsLoaded || !renamesLoaded)
      throw Error("load_failed",
                  "Could not reload annotations or renames sidecar",
                  {{"annotations_loaded", annotationsLoaded},
                   {"renames_loaded", renamesLoaded},
                   {"state_changed", true}});
    dirty_ = false;
    return metadata();
  }
  if (operation != "disasm" && operation != "bytes" &&
      operation != "annotation_set" && operation != "rename" &&
      operation != "decompile" && operation != "cfg" &&
      operation != "cfg_summary" && operation != "cfg_viewport" &&
      operation != "xrefs")
    throw Error("unsupported", "Unknown worker operation: " + operation);
  const auto address = parseAddress(stringField(p, "address"));
  if (operation == "disasm") {
    const auto limit = sizeField(p, "limit", 128, 512);
    if (!limit)
      throw Error("invalid_request", "limit must be at least 1");
    auto items = backendJson(
        neverd_disasm_json(session_, address, static_cast<int>(limit)), true);
    for (auto &item : items) {
      item["address"] = item.at("addr");
      item["operands"] = item.value("op_str", "");
      item["comment"] = ownedString(neverd_annotation_get(
          session_, parseAddress(item.at("addr").get<std::string>())));
      item.erase("addr");
      item.erase("op_str");
    }
    Json next = nullptr;
    if (!items.empty()) {
      const auto last =
          parseAddress(items.back().at("address").get<std::string>());
      const auto size = items.back().at("size").get<std::uint64_t>();
      if (size && size <= std::numeric_limits<std::uint64_t>::max() - last &&
          items.size() == limit)
        next = hexAddress(last + size);
    }
    return {{"items", items},
            {"address", hexAddress(address)},
            {"next_address", next},
            {"complete", next.is_null()}};
  }
  if (operation == "bytes") {
    auto size = sizeField(p, "size", 256, 65536);
    if (!size)
      throw Error("invalid_request", "size must be at least 1");
    if (size - 1 > std::numeric_limits<std::uint64_t>::max() - address)
      throw Error("invalid_address", "Byte range overflows the address space");
    std::vector<unsigned char> buffer(size);
    const int count = neverd_read_bytes(session_, address, buffer.data(),
                                        static_cast<int>(size));
    std::string data;
    constexpr char digits[] = "0123456789abcdef";
    for (int i = 0; i < count; ++i) {
      data += digits[buffer[i] >> 4];
      data += digits[buffer[i] & 15];
    }
    // The bytes as text in an encoding, one cell per byte, for a hex view.
    Json cells;
    if (p.contains("text_encoding")) {
      const auto encoding = stringField(p, "text_encoding", {}, 64);
      static const auto decode =
          engineSymbol<DecodeTextFunction>("neverd_decode_text_json");
      if (!decode)
        throw Error("unsupported", "The engine cannot decode text");
      const char *raw = decode(buffer.data(), count, encoding.c_str());
      if (!raw)
        throw Error("unsupported_encoding",
                    "Unknown text encoding: " + encoding);
      auto decoded = backendJson(raw);
      if (!decoded.is_object() || !decoded["cells"].is_array() ||
          decoded["cells"].size() != static_cast<std::size_t>(count))
        throw Error("engine_error", "Engine returned malformed text cells");
      cells = std::move(decoded["cells"]);
    }
    Json result = {
        {"address", hexAddress(address)},
        {"encoding", "hex"},
        {"data", data},
        {"bytes_read", count},
        {"requested_size", size},
        {"mapping_status", count == 0 ? "unmapped_or_unmaterialized"
                           : count < static_cast<int>(size) ? "partial"
                                                            : "mapped"},
        {"next_address",
         count > 0 && static_cast<std::uint64_t>(count) <=
                          std::numeric_limits<std::uint64_t>::max() - address
             ? Json(hexAddress(address + count))
             : Json(nullptr)}};
    if (!cells.is_null())
      result["cells"] = std::move(cells);
    return result;
  }
  if (operation == "annotation_set") {
    requireWriter();
    auto text = stringField(p, "text", {}, 65536);
    auto &store = history();
    store.stage(
        {{"kind", "annotation"},
         {"address", hexAddress(address)},
         {"before", ownedString(neverd_annotation_get(session_, address))},
         {"after", text}});
    neverd_annotation_set(session_, address, text.c_str());
    dirty_ = true;
    invalidate();
    ++revision_;
    return {{"address", hexAddress(address)},
            {"text", text},
            {"dirty", true},
            {"saved", false}};
  }
  if (operation == "rename") {
    requireWriter();
    if (dirty_)
      throw Error(
          "unsaved_changes",
          "Save or reload staged annotations before renaming a function");
    const auto name = stringField(p, "name", {}, 4096);
    if (name.empty())
      throw Error("invalid_request", "Function name cannot be empty");
    const int index = neverd_func_find_by_addr(session_, address);
    if (index < 0)
      throw Error("not_found", "No function begins at that address");
    auto &store = history();
    auto state = store.committedState();
    auto &renames = state["renames"];
    Json before = nullptr;
    bool found = false;
    for (auto &item : renames)
      if (parseAddress(item.at("addr").get<std::string>()) == address) {
        before = item;
        item["renamed"] = name;
        found = true;
      }
    if (!found)
      renames.push_back(
          {{"addr", hexAddress(address)},
           {"original", ownedString(neverd_func_name(session_, index))},
           {"renamed", name}});
    Json after;
    for (const auto &item : renames)
      if (parseAddress(item.at("addr").get<std::string>()) == address)
        after = item;
    const auto checkpoint = store;
    store.stage({{"kind", "rename"},
                 {"address", hexAddress(address)},
                 {"before", before},
                 {"after", after}});
    try {
      store.persist(state);
    } catch (...) {
      store = checkpoint;
      throw;
    }
    if (neverd_renames_load(session_) != 0)
      throw Error("reload_failed", "Rename was saved but could not be reloaded",
                  {{"saved", true}});
    invalidate();
    ++revision_;
    return {{"address", hexAddress(address)}, {"name", name}, {"saved", true}};
  }
  if (operation == "decompile") {
    const auto representation = stringField(p, "representation", "c", 16);
    if (representation != "c" && representation != "llvmc" &&
        representation != "low" && representation != "med" &&
        representation != "high" && representation != "llvm")
      throw Error("unsupported", "Unknown code representation");
    const auto offset =
        sizeField(p, "offset", 0, std::numeric_limits<std::size_t>::max());
    const auto limit = sizeField(p, "limit", 512, 2048);
    if (!limit)
      throw Error("invalid_request", "limit must be at least 1");
    prepareFunction(address);
    std::string mappingStatus = "unsupported_representation";
    if (representation == "low" || representation == "med" ||
        representation == "c" || representation == "llvmc") {
      if (auto named = namedViewPage(address, representation, offset, limit))
        return *named;
      if (const auto view = irViewFunction()) {
        auto result = backendJson(
            view(session_, address, representation.c_str(), offset, limit),
            true);
        mappingStatus =
            result.value("mapping_status", std::string("unavailable"));
        if (result.contains("text")) {
          if (!result["text"].is_string() ||
              result["text"].get_ref<const std::string &>().size() >
                  2 * 1024 * 1024 ||
              !result.contains("rows") || !result["rows"].is_array() ||
              result["rows"].size() > limit)
            throw Error("invalid_engine_result",
                        "Invalid bounded IR view page");
          result["project_id"] = projectId_;
          result["revision"] = revision();
          return result;
        }
      } else
        mappingStatus = "unavailable_engine_api";
    }
    const auto key = hexAddress(address) + ":" + representation;
    if (textKey_ != key) {
      textKey_.clear();
      const char *result =
          representation == "c"       ? neverd_decompile(session_, address)
          : representation == "llvmc" ? neverd_decompile_llvm(session_, address)
          : representation == "low"   ? neverd_ir_low(session_, address)
          : representation == "med"   ? neverd_ir_med(session_, address)
          : representation == "high"  ? neverd_ir_high(session_, address)
                                      : neverd_ir_llvm(session_, address);
      textCache_ = ownedString(result);
      if (textCache_.empty())
        throw Error("unavailable", error().empty()
                                       ? "This representation is unavailable"
                                       : error());
      if (const auto &aliases = listing().functionAliases(); !aliases.empty()) {
        std::vector<std::pair<std::size_t, std::ptrdiff_t>> shift;
        textCache_ = renameIdentifiers(textCache_, aliases, shift);
      }
      textLines_.clear();
      textLines_.push_back(0);
      for (std::size_t i = 0; i < textCache_.size(); ++i)
        if (textCache_[i] == '\n' && i + 1 < textCache_.size())
          textLines_.push_back(i + 1);
      textKey_ = key;
    }
    const auto start = std::min(offset, textLines_.size());
    const auto end = start + std::min(limit, textLines_.size() - start);
    const auto startByte =
        start < textLines_.size() ? textLines_[start] : textCache_.size();
    const auto endByte =
        end < textLines_.size() ? textLines_[end] : textCache_.size();
    if (endByte - startByte > 2 * 1024 * 1024)
      throw Error("budget_exceeded",
                  "Code page exceeds 2 MiB; request fewer lines");
    return {
        {"address", hexAddress(address)},
        {"representation", representation},
        {"text", textCache_.substr(startByte, endByte - startByte)},
        {"offset", offset},
        {"total_lines", textLines_.size()},
        {"complete", end == textLines_.size()},
        {"next_offset", end == textLines_.size() ? Json(nullptr) : Json(end)},
        {"mapping_status", mappingStatus},
        {"provenance_complete", false},
        {"rows", Json::array()},
        {"project_id", projectId_},
        {"revision", revision()}};
  }
  if (operation == "cfg_summary") {
    prepareFunction(address);
    const auto key = hexAddress(address);
    // Client text metrics size each node to its formatted listing rows.
    GraphMetrics metrics;
    if (const auto it = p.find("metrics"); it != p.end() && it->is_object()) {
      const auto number = [&](const char *field) {
        const auto value = it->find(field);
        if (value == it->end())
          return 0.0;
        if (!value->is_number() || !std::isfinite(value->get<double>()) ||
            value->get<double>() < 0 || value->get<double>() > 1000)
          throw Error("invalid_request",
                      std::string("metrics.") + field + " is out of range");
        return value->get<double>();
      };
      metrics.charWidth = number("char_width");
      metrics.lineHeight = number("line_height");
      metrics.padding = number("padding");
      metrics.titleHeight = number("title_height");
    }
    const auto metricsKey = std::to_string(metrics.charWidth) + "/" +
                            std::to_string(metrics.lineHeight) + "/" +
                            std::to_string(metrics.padding) + "/" +
                            std::to_string(metrics.titleHeight);
    if (!graph_ || graph_->address() != key || graphMetrics_ != metricsKey) {
      auto snapshot = std::make_unique<GraphSnapshot>(
          backendJson(neverd_cfg_json(session_, address), true), key,
          projectId_ + ":" + revision() + ":" + key + ":" + metricsKey, metrics,
          metrics.valid()
              ? GraphRows([this](std::uint64_t start, std::uint64_t end) {
                  return listing().blockLines(start, end);
                })
              : GraphRows());
      graph_ = std::move(snapshot);
      graphMetrics_ = metricsKey;
    }
    return graph_->summary();
  }
  if (operation == "cfg_viewport") {
    if (!graph_ || graph_->address() != hexAddress(address))
      throw Error("stale_layout",
                  "No matching graph snapshot; request cfg_summary first");
    return graph_->viewport(p);
  }
  if (operation == "cfg") {
    prepareFunction(address);
    auto result = backendJson(neverd_cfg_json(session_, address), true);
    if (result.value("nodes", Json::array()).size() > 500 ||
        result.value("edges", Json::array()).size() > 2000)
      throw Error("budget_exceeded",
                  "Graph exceeds the preview budget of 500 nodes / 2000 edges; "
                  "no partial graph was published");
    for (auto &node : result["nodes"]) {
      if (!node["id"].is_string())
        node["id"] = node["id"].dump();
      node["address"] = node.at("start");
      node["label"] = node.at("start");
      node["lines"] = node.at("disasm");
    }
    for (auto &edge : result["edges"]) {
      if (!edge["from"].is_string())
        edge["from"] = edge["from"].dump();
      if (!edge["to"].is_null() && !edge["to"].is_string())
        edge["to"] = edge["to"].dump();
    }
    result["complete"] = true;
    return result;
  }
  if (operation == "xrefs") {
    const auto direction = stringField(p, "direction", "to", 8);
    if (direction != "to" && direction != "from")
      throw Error("invalid_request", "direction must be to or from");
    if (stringField(p, "source", "direct", 16) != "ir")
      return listing().references(address, p);
    analyze();
    const auto arch = folded(ownedString(neverd_session_arch_name(session_)));
    if (arch == "evm" || arch == "sbf")
      throw Error("unsupported",
                  "The existing xref API does not expose architecture-specific "
                  "references for this image");
    auto items = backendJson(direction == "to"
                                 ? neverd_xrefs_to_json(session_, address)
                                 : neverd_xrefs_from_json(session_, address),
                             true);
    for (auto &item : items) {
      item["address"] = item.at(direction == "to" ? "from" : "to");
      item["function"] = item.value("func", "");
      item["kind"] = "IR constant reference";
    }
    return page(items, p);
  }
  throw Error("unsupported", "Unknown worker operation: " + operation);
}
} // namespace neverd::worker
