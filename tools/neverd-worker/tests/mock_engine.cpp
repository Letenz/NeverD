// Deterministic C ABI fake for protocol/lifecycle tests; never linked into the
// shipped worker. It deliberately logs through ordinary stdout during work.
#define NEVERD_EXPORTS 1
#include "Protocol.h"

#include "neverd/sdk/NeverDCAPIDisasm.h"
#include "neverd/sdk/NeverDCAPIPersist.h"
#include "neverd/sdk/NeverDCAPIQuery.h"
#include "neverd/sdk/NeverDCAPISession.h"
#include "neverd/sdk/NeverDCAPISigs.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <thread>

using neverd::worker::hexAddress;
using neverd::worker::Json;
using neverd::worker::parseAddress;
namespace {
constexpr std::uint64_t Base = 0xffff800012340000ULL;
struct MockSession {
  std::string path, error;
  std::map<std::uint64_t, std::string> annotations, names;
  neverd_load_progress_fn progress = nullptr;
  void *progressUser = nullptr;
};
MockSession *session(neverd_session_t s) {
  return static_cast<MockSession *>(s);
}
const char *copy(std::string value) {
  char *result = static_cast<char *>(std::malloc(value.size() + 1));
  std::memcpy(result, value.c_str(), value.size() + 1);
  return result;
}
Json read(const std::string &path) {
  std::ifstream input(path);
  if (!input)
    return Json::array();
  return Json::parse(input);
}
} // namespace
extern "C" {
neverd_session_t neverd_session_create() {
  std::puts("native stdout: session created");
  std::fflush(stdout);
  return new MockSession;
}
void neverd_session_destroy(neverd_session_t s) { delete session(s); }
void neverd_session_set_load_progress(neverd_session_t s,
                                      neverd_load_progress_fn callback,
                                      void *userData) {
  if (s) {
    session(s)->progress = callback;
    session(s)->progressUser = userData;
  }
}
int neverd_session_load(neverd_session_t s, const char *path) {
  const auto notify = [&](const char *phase, unsigned long long done) {
    if (session(s)->progress)
      session(s)->progress(session(s)->progressUser, phase, done, 1, path);
  };
  notify("image", 0);
  if (std::string(path).find("bad-input") != std::string::npos) {
    session(s)->error = "mock load failure";
    return 0;
  }
  session(s)->path = path;
  notify("ready", 1);
  return 1;
}
int neverd_session_is_loaded(neverd_session_t s) {
  return !session(s)->path.empty();
}
int neverd_apply_signature_file(neverd_session_t s, const char *path) {
  session(s)->error.clear();
  if (!std::ifstream(path)) {
    session(s)->error = "mock signature file is missing";
    return -1;
  }
  return 0;
}
int neverd_auto_apply_signatures(neverd_session_t s, const char *path) {
  return neverd_apply_signature_file(s, path);
}
int neverd_session_analyze(neverd_session_t) {
  std::puts("python-style print during analysis");
  std::fflush(stdout);
  std::this_thread::sleep_for(std::chrono::milliseconds(1800));
  return 1;
}
const char *neverd_session_file_path(neverd_session_t s) {
  return copy(session(s)->path);
}
const char *neverd_session_arch_name(neverd_session_t) {
  return copy("x86_64");
}
const char *neverd_session_format_name(neverd_session_t) { return copy("ELF"); }
int neverd_session_bitness(neverd_session_t) { return 64; }
unsigned long long neverd_session_file_size(neverd_session_t) { return 8192; }
neverd_va_t neverd_session_base_addr(neverd_session_t) { return Base; }
neverd_va_t neverd_session_entry_addr(neverd_session_t) { return Base; }
int neverd_session_segment_count(neverd_session_t) { return 1; }
int neverd_session_section_count(neverd_session_t) { return 1; }
int neverd_session_import_count(neverd_session_t) { return 0; }
int neverd_session_export_count(neverd_session_t) { return 1; }
int neverd_session_symbol_count(neverd_session_t) { return 600; }
const char *neverd_last_error(neverd_session_t s) {
  return copy(session(s)->error);
}
void neverd_free_string(const char *value) {
  std::free(const_cast<char *>(value));
}
const char *neverd_version_number() { return copy("test-1.0"); }
const char *neverd_dashboard_json(neverd_session_t s) {
  // Deterministic fixture-only identity. The real engine uses SHA-256; the
  // production real-engine test verifies that digest against Python hashlib.
  std::ifstream input(session(s)->path, std::ios::binary);
  std::uint64_t value = 1469598103934665603ULL;
  char byte;
  while (input.get(byte)) {
    value ^= static_cast<unsigned char>(byte);
    value *= 1099511628211ULL;
  }
  auto digest = hexAddress(value).substr(2);
  digest.insert(0, 16 - digest.size(), '0');
  return copy(
      Json{{"hashes", {{"sha256", digest + digest + digest + digest}}}}.dump());
}
int neverd_func_count(neverd_session_t) { return 600; }
neverd_va_t neverd_func_entry(neverd_session_t, int index) {
  return Base + 16 * index;
}
int neverd_func_size(neverd_session_t, int) { return 16; }
const char *neverd_func_name(neverd_session_t s, int index) {
  const auto address = Base + 16 * index;
  auto it = session(s)->names.find(address);
  return copy(it == session(s)->names.end()
                  ? "function_" + std::to_string(index)
                  : it->second);
}
int neverd_func_find_by_addr(neverd_session_t, neverd_va_t address) {
  return address >= Base && address - Base < 9600 && (address - Base) % 16 == 0
             ? static_cast<int>((address - Base) / 16)
             : -1;
}
int neverd_func_find_by_name(neverd_session_t s, const char *name) {
  for (int i = 0; i < 600; ++i) {
    const auto *value = neverd_func_name(s, i);
    const bool match = std::string(value) == name;
    neverd_free_string(value);
    if (match)
      return i;
  }
  return -1;
}
const char *neverd_resolve_addr(neverd_session_t s, neverd_va_t address) {
  const auto index = neverd_func_find_by_addr(s, address);
  if (index < 0)
    return nullptr;
  const auto *raw = neverd_func_name(s, index);
  const std::string name(raw);
  neverd_free_string(raw);
  return copy(Json{
      {"type", "function"},
      {"addr", hexAddress(address)},
      {"name", name},
      {"display_name", name},
      {"linkage_name",
       name}}.dump());
}
int neverd_read_bytes(neverd_session_t, neverd_va_t address,
                      unsigned char *buffer, int size) {
  if (address < Base || address - Base >= 9600 || size < 0)
    return 0;
  // Each 16-byte function body is the byte ramp 00 01 02 ... 0f.
  const auto available =
      static_cast<int>(std::min<std::uint64_t>(size, Base + 9600 - address));
  for (int i = 0; i < available; ++i)
    buffer[i] = static_cast<unsigned char>((address - Base + i) & 0xf);
  return available;
}
// Each 16-byte fixture function is one-byte instructions: nops, a jump back
// to its entry at offset 8 and a return at offset 15.
constexpr std::uint64_t JumpOffset = 8, ReturnOffset = 15;
Json fixtureInstructions(neverd_va_t address, int count, bool flow) {
  Json result = Json::array();
  for (int i = 0; i < count && address >= Base && address + i - Base < 9600;
       ++i) {
    const auto at = address + i;
    const auto offset = (at - Base) % 16;
    const auto entry = at - offset;
    Json row = {{"addr", hexAddress(at)},
                {"size", 1},
                {"mnemonic", "nop"},
                {"op_str", ""},
                {"bytes", "90"}};
    if (offset == JumpOffset) {
      row["mnemonic"] = "jmp";
      row["op_str"] = hexAddress(entry);
      row["bytes"] = "eb";
      if (flow) {
        row["flow"] = "jump";
        row["target"] = hexAddress(entry);
      }
    } else if (offset == ReturnOffset) {
      row["mnemonic"] = "ret";
      row["bytes"] = "c3";
      if (flow)
        row["flow"] = "ret";
    }
    result.push_back(std::move(row));
  }
  return result;
}
const char *neverd_disasm_json(neverd_session_t s, neverd_va_t address,
                               int count) {
  session(s)->error.clear();
  return copy(fixtureInstructions(address, count, false).dump());
}
const char *neverd_disasm_json_ex(neverd_session_t s, neverd_va_t address,
                                  int count, unsigned options) {
  session(s)->error.clear();
  return copy(
      fixtureInstructions(address, count, options & NEVERD_DISASM_FLOW).dump());
}
const char *neverd_code_refs_json(neverd_session_t s, neverd_va_t firstEntry,
                                  int maxFunctions) {
  session(s)->error.clear();
  maxFunctions = std::clamp(maxFunctions, 1, 4096);
  Json refs = Json::array();
  int first =
      firstEntry <= Base ? 0 : static_cast<int>((firstEntry - Base + 15) / 16);
  int index = first;
  for (; index < 600 && index < first + maxFunctions; ++index) {
    const auto entry = Base + 16 * static_cast<std::uint64_t>(index);
    refs.push_back({hexAddress(entry + JumpOffset), hexAddress(entry), "jump"});
  }
  return copy(
      Json{{"refs", refs},
           {"next_entry",
            index < 600 ? Json(hexAddress(Base + 16 * index)) : Json(nullptr)},
           {"function_count", 600}}
          .dump());
}
const char *neverd_decompile(neverd_session_t, neverd_va_t) {
  std::string text;
  for (int i = 0; i < 700; ++i)
    text += "// code line " + std::to_string(i) + "\n";
  return copy(text);
}
const char *neverd_ir_low(neverd_session_t s, neverd_va_t a) {
  return neverd_decompile(s, a);
}
const char *neverd_ir_med(neverd_session_t s, neverd_va_t a) {
  return neverd_decompile(s, a);
}
const char *neverd_ir_high(neverd_session_t s, neverd_va_t a) {
  return neverd_decompile(s, a);
}
const char *neverd_ir_llvm(neverd_session_t s, neverd_va_t a) {
  return neverd_decompile(s, a);
}
const char *neverd_decompile_llvm(neverd_session_t s, neverd_va_t a) {
  return neverd_decompile(s, a);
}
const char *neverd_ir_view_json(neverd_session_t s, neverd_va_t address,
                                const char *representation, std::size_t offset,
                                std::size_t limit) {
  session(s)->error.clear();
  // This mock deliberately represents an older page API that maps Low/Med
  // only. C requests must exercise the worker's legacy fallback.
  if (std::string(representation) != "low" &&
      std::string(representation) != "med")
    return copy(Json{{"mapping_status", "unsupported_representation"},
                     {"rows", Json::array()}}
                    .dump());
  Json rows = Json::array();
  std::string text;
  const auto end =
      std::min<std::size_t>(700, std::min<std::size_t>(offset, 700) + limit);
  for (auto i = offset; i < end; ++i) {
    text += "// code line " + std::to_string(i) + "\n";
    rows.push_back(
        {{"line", i},
         {"object_id",
          std::string(representation) + ":op:" + std::to_string(i)},
         {"kind", i == 0 ? "header" : "operation"},
         {"mapping_status", i == 0 ? "unmapped" : "instruction_anchor"},
         {"addresses",
          i == 0 ? Json::array() : Json::array({hexAddress(address + i)})}});
  }
  return copy(Json{{"schema_version", 1},
                   {"address", hexAddress(address)},
                   {"representation", representation},
                   {"text", text},
                   {"rows", rows},
                   {"mapping_status", "instruction_anchors"},
                   {"provenance_complete", false},
                   {"offset", offset},
                   {"total_lines", 700},
                   {"next_offset", end == 700 ? Json(nullptr) : Json(end)},
                   {"complete", end == 700}}
                  .dump());
}
const char *neverd_strings_json(neverd_session_t, int) {
  Json items = Json::array();
  for (int i = 0; i < 600; ++i)
    items.push_back({{"addr", hexAddress(Base + i)},
                     {"value", "string " + std::to_string(i)},
                     {"length", 10}});
  return copy(items.dump());
}
const char *neverd_segments_json(neverd_session_t) {
  return copy(Json::array({{{"name", ".text"},
                            {"va", hexAddress(Base)},
                            {"size", "0x2580"},
                            {"flags", "R-X"}}})
                  .dump());
}
const char *neverd_sections_json(neverd_session_t) {
  // Spelled like the engine: numeric sizes and R/W/X flags.
  return copy(Json::array({{{"name", ".text"},
                            {"segment", ".text"},
                            {"va", hexAddress(Base)},
                            {"size", 0x2580},
                            {"file_off", 0x1000},
                            {"file_sz", 0x2580},
                            {"alignment", 16},
                            {"flags", "R-X"}}})
                  .dump());
}
const char *neverd_symbols_json(neverd_session_t) {
  return copy(Json::array({{{"addr", hexAddress(Base)},
                            {"name", "function_0"},
                            {"type", "function"}}})
                  .dump());
}
const char *neverd_imports_json(neverd_session_t) {
  return copy(Json::array().dump());
}
const char *neverd_exports_json(neverd_session_t) {
  return copy(Json::array({{{"addr", hexAddress(Base)},
                            {"name", "function_0"},
                            {"ordinal", 1}}})
                  .dump());
}
const char *neverd_entrypoints_json(neverd_session_t) {
  return copy(
      Json::array(
          {{{"addr", hexAddress(Base)}, {"name", "start"}, {"type", "entry"}}})
          .dump());
}
// The fixture's "library" match names the second function.
const char *neverd_sig_matches_json(neverd_session_t) {
  return copy(Json::array({{{"addr", hexAddress(Base + 16)},
                            {"name", "function_1"},
                            {"source", "mock"}}})
                  .dump());
}
void neverd_session_restrict_function(neverd_session_t, neverd_va_t) {}
const char *neverd_search_bytes(neverd_session_t, const unsigned char *pattern,
                                int length, int limit) {
  Json hits = Json::array();
  if (pattern && length > 0 && length <= 16 && pattern[0] + length <= 16) {
    bool ramp = true;
    for (int i = 1; i < length; ++i)
      ramp = ramp && pattern[i] == pattern[0] + i;
    for (int i = 0; ramp && i < 600 && static_cast<int>(hits.size()) < limit;
         ++i)
      hits.push_back({{"addr", hexAddress(Base + 16 * i + pattern[0])}});
  }
  return copy(hits.dump());
}
const char *neverd_search_string(neverd_session_t, const char *pattern,
                                 int caseSensitive, int limit) {
  Json hits = Json::array();
  const auto fold = [caseSensitive](std::string text) {
    if (!caseSensitive)
      for (auto &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
  };
  const auto needle = fold(pattern ? pattern : "");
  for (int i = 0;
       !needle.empty() && i < 600 && static_cast<int>(hits.size()) < limit;
       ++i) {
    const auto value = "string " + std::to_string(i);
    if (fold(value).find(needle) != std::string::npos)
      hits.push_back({{"addr", hexAddress(Base + i)}, {"value", value}});
  }
  return copy(hits.dump());
}
const char *neverd_xrefs_to_json(neverd_session_t s, neverd_va_t address) {
  session(s)->error.clear();
  return copy(Json::array({{{"from", hexAddress(address + 8)},
                            {"func", "function_0"},
                            {"block", 0}}})
                  .dump());
}
const char *neverd_xrefs_from_json(neverd_session_t s, neverd_va_t address) {
  session(s)->error.clear();
  return copy(Json::array({{{"to", hexAddress(address + 8)},
                            {"func", "function_0"},
                            {"opcode", "COPY"}}})
                  .dump());
}
const char *neverd_cfg_json(neverd_session_t s, neverd_va_t address) {
  session(s)->error.clear();
  Json nodes = Json::array(), edges = Json::array();
  const int count =
      address == Base + 2 ? 10000 : (address == Base + 1 ? 501 : 2);
  for (int i = 0; i < count; ++i) {
    nodes.push_back({{"id", i},
                     {"start", hexAddress(address + i)},
                     {"end", hexAddress(address + i + 1)},
                     {"insn_count", 1},
                     {"disasm", {"nop"}}});
    if (i)
      edges.push_back({{"from", i - 1}, {"to", i}, {"type", "unconditional"}});
  }
  return copy(Json{{"nodes", nodes}, {"edges", edges}}.dump());
}
void neverd_annotation_set(neverd_session_t s, neverd_va_t address,
                           const char *text) {
  if (!text || !*text)
    session(s)->annotations.erase(address);
  else
    session(s)->annotations[address] = text;
}
const char *neverd_annotation_get(neverd_session_t s, neverd_va_t address) {
  auto it = session(s)->annotations.find(address);
  return it == session(s)->annotations.end() ? nullptr : copy(it->second);
}
const char *neverd_annotations_json(neverd_session_t s) {
  Json result = Json::array();
  for (const auto &[address, text] : session(s)->annotations)
    result.push_back({{"addr", hexAddress(address)}, {"text", text}});
  return copy(result.dump());
}
int neverd_annotations_load(neverd_session_t s) {
  try {
    auto values = read(session(s)->path + ".neverd-annotations.json");
    session(s)->annotations.clear();
    for (const auto &item : values)
      session(s)
          ->annotations[parseAddress(item.at("addr").get<std::string>())] =
          item.at("text");
    return 0;
  } catch (...) {
    return 1;
  }
}
const char *neverd_renames_json(neverd_session_t s) {
  Json result = Json::array();
  for (const auto &[address, name] : session(s)->names)
    result.push_back({{"addr", hexAddress(address)},
                      {"renamed", name},
                      {"original", "function_0"}});
  return copy(result.dump());
}
int neverd_renames_load(neverd_session_t s) {
  try {
    auto values = read(session(s)->path + ".neverd-renames.json");
    session(s)->names.clear();
    for (const auto &item : values)
      session(s)->names[parseAddress(item.at("addr").get<std::string>())] =
          item.at("renamed");
    return 0;
  } catch (...) {
    return 1;
  }
}
}
