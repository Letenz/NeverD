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
#include <string>
#include <thread>
#include <vector>

using neverd::worker::hexAddress;
using neverd::worker::Json;
using neverd::worker::parseAddress;
namespace {
constexpr std::uint64_t Base = 0xffff800012340000ULL;
// A read-only data section of relocated pointers: slot i points to function
// i + 1.
constexpr std::uint64_t DataBase = Base + 0x3000, DataSlots = 8;
// Read-only data holding a UTF-8 and a UTF-16LE string.
constexpr std::uint64_t RodataBase = Base + 0x3100, RodataSize = 0x20;
/// The pointer data slot \p index holds: function_(index + 1), except the
/// last slot, which holds the UTF-16LE string.
std::uint64_t slotTarget(std::uint64_t index) {
  return index == DataSlots - 1 ? RodataBase + 8 : Base + 16 * (index + 1);
}
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
int neverd_session_segment_count(neverd_session_t) { return 2; }
int neverd_session_section_count(neverd_session_t) { return 2; }
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
// The fixture's detector finds nothing the image does not list.
int neverd_session_discover_functions(neverd_session_t) { return 600; }
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
  if (size >= 0 && address >= RodataBase && address - RodataBase < RodataSize) {
    // "\u4e2d\u6587" in UTF-8 at +0 and "Wide" in UTF-16LE at +8, zero filled.
    static constexpr unsigned char Rodata[RodataSize] = {
        0xe4, 0xb8, 0xad, 0xe6, 0x96, 0x87, 0, 0, 'W', 0, 'i', 0, 'd', 0, 'e'};
    const auto available = static_cast<int>(
        std::min<std::uint64_t>(size, RodataBase + RodataSize - address));
    std::copy_n(Rodata + (address - RodataBase), available, buffer);
    return available;
  }
  if (size >= 0 && address >= DataBase && address - DataBase < DataSlots * 8) {
    const auto available = static_cast<int>(
        std::min<std::uint64_t>(size, DataBase + DataSlots * 8 - address));
    for (int i = 0; i < available; ++i) {
      const auto offset = address - DataBase + i;
      const std::uint64_t pointer = slotTarget(offset / 8);
      buffer[i] = static_cast<unsigned char>(pointer >> (8 * (offset % 8)));
    }
    return available;
  }
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
/// Functions with stack frames: offset -> mnemonic, operands, stack pointer
/// move and, for a conditional jump, its target offset.
struct FrameRow {
  const char *mnemonic, *operands;
  int move;
  int branch = -1;
};
/// function_5 keeps a frame without a frame pointer.  Its loop back to the
/// entry restores the stack pointer, and the code after the loop is
/// unreachable.
const std::map<std::uint64_t, FrameRow> FrameFunction = {
    {0, {"push", "rbx", -8}},
    {1, {"sub", "rsp, 0x20", -0x20}},
    {2, {"mov", "qword ptr [rsp + 0x18], rax", 0}},
    {3, {"lea", "rdi, [rsp + 8]", 0}},
    {4, {"mov", "rax, qword ptr [rsp + 0x30]", 0}},
    {5, {"movups", "xmmword ptr [rsp + 8], xmm0", 0}},
    {6, {"add", "rsp, 0x20", 0x20}},
    {7, {"pop", "rbx", 8}},
    {9, {"mov", "rax, qword ptr [rsp + 8]", 0}},
};
constexpr std::uint64_t FrameFunctionEntry = Base + 0x50;
/// function_6 reaches offset 4 both past a push and around it, so the stack
/// pointer's distance there is unknown.
const std::map<std::uint64_t, FrameRow> JoinFunction = {
    {0, {"push", "rbx", -8}},
    {1, {"mov", "rax, qword ptr [rsp]", 0}},
    {2, {"je", "4", 0, 4}},
    {3, {"push", "rcx", -8}},
    {4, {"mov", "rax, qword ptr [rsp + 8]", 0}},
    {7, {"ret", "", 0}},
};
constexpr std::uint64_t JoinFunctionEntry = Base + 0x60;

Json fixtureInstructions(neverd_va_t address, int count, bool flow,
                         bool stack) {
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
    if (stack)
      row["sp"] = 0;
    const auto *rows = entry == FrameFunctionEntry  ? &FrameFunction
                       : entry == JoinFunctionEntry ? &JoinFunction
                                                    : nullptr;
    if (const auto frame = rows ? rows->find(offset) : FrameFunction.end();
        rows && frame != rows->end()) {
      row["mnemonic"] = frame->second.mnemonic;
      row["op_str"] = frame->second.operands;
      if (stack)
        row["sp"] = frame->second.move;
      if (flow && frame->second.branch >= 0) {
        row["op_str"] = hexAddress(entry + frame->second.branch);
        row["flow"] = "cjump";
        row["target"] = hexAddress(entry + frame->second.branch);
      } else if (flow && std::string_view(frame->second.mnemonic) == "ret") {
        row["flow"] = "ret";
      }
    }
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
  return copy(fixtureInstructions(address, count, false, false).dump());
}
const char *neverd_disasm_json_ex(neverd_session_t s, neverd_va_t address,
                                  int count, unsigned options) {
  session(s)->error.clear();
  return copy(fixtureInstructions(address, count, options & NEVERD_DISASM_FLOW,
                                  options & NEVERD_DISASM_STACK)
                  .dump());
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
    // function_3 calls function_2 through data slot 1.
    if (index == 3)
      refs.push_back({hexAddress(entry + 4), hexAddress(Base + 0x20), "icall"});
    // function_7 takes the UTF-8 string's address, reads the slot that
    // holds the UTF-16LE string's, and points into both strings: at the
    // second character of the UTF-8 one and at an odd byte of the other.
    if (index == 7) {
      refs.push_back({hexAddress(entry + 2), hexAddress(RodataBase), "offset"});
      refs.push_back({hexAddress(entry + 3),
                      hexAddress(DataBase + (DataSlots - 1) * 8), "read"});
      refs.push_back(
          {hexAddress(entry + 4), hexAddress(RodataBase + 3), "offset"});
      refs.push_back(
          {hexAddress(entry + 5), hexAddress(RodataBase + 11), "offset"});
    }
  }
  return copy(
      Json{{"refs", refs},
           {"next_entry",
            index < 600 ? Json(hexAddress(Base + 16 * index)) : Json(nullptr)},
           {"function_count", 600}}
          .dump());
}
const char *neverd_string_refs_json(neverd_session_t s, const char *options,
                                    neverd_va_t firstEntry, int maxFunctions) {
  session(s)->error.clear();
  const Json parsed =
      options ? Json::parse(options, nullptr, false) : Json::object();
  const auto wanted = [&](const char *encoding) {
    if (!parsed.contains("encodings"))
      return true;
    for (const auto &name : parsed["encodings"])
      if (name == encoding)
        return true;
    return false;
  };
  const auto minimum = parsed.value("min_length", 4);
  // function_7's references, as the engine joins them with the strings: the
  // UTF-8 string whole and from its second character ("\u6587"), and the
  // UTF-16LE one through data slot 7 and from its third character ("de").
  constexpr std::uint64_t Entry = Base + 16 * 7;
  const std::uint64_t slot = DataBase + (DataSlots - 1) * 8;
  Json refs = Json::array();
  if (firstEntry <= Entry && maxFunctions >= 1) {
    if (wanted("utf-8"))
      refs.push_back({hexAddress(Entry + 2), hexAddress(RodataBase),
                      hexAddress(RodataBase), 0, "offset", nullptr, "nop"});
    if (wanted("utf-16le"))
      refs.push_back({hexAddress(Entry + 3), hexAddress(RodataBase + 8),
                      hexAddress(RodataBase + 8), 0, "read", hexAddress(slot),
                      "nop"});
    if (wanted("utf-8") && minimum <= 1)
      refs.push_back({hexAddress(Entry + 4), hexAddress(RodataBase + 3),
                      hexAddress(RodataBase), 3, "offset", nullptr, "nop"});
    if (wanted("utf-16le") && minimum <= 2)
      refs.push_back({hexAddress(Entry + 5), hexAddress(RodataBase + 11),
                      hexAddress(RodataBase + 8), 2, "offset", nullptr, "nop"});
  }
  return copy(
      Json{{"refs", refs}, {"next_entry", nullptr}, {"function_count", 600}}
          .dump());
}
const char *neverd_pointer_refs_json(neverd_session_t s, neverd_va_t firstSlot,
                                     int maxSlots) {
  session(s)->error.clear();
  Json refs = Json::array();
  std::uint64_t slot = std::max<std::uint64_t>(firstSlot, DataBase);
  slot = DataBase + (slot - DataBase + 7) / 8 * 8;
  for (int count = 0; slot < DataBase + DataSlots * 8 && count < maxSlots;
       ++count, slot += 8)
    refs.push_back({hexAddress(slot),
                    hexAddress(slotTarget((slot - DataBase) / 8)), "offset"});
  return copy(Json{{"refs", refs},
                   {"next_slot", slot < DataBase + DataSlots * 8
                                     ? Json(hexAddress(slot))
                                     : Json(nullptr)}}
                  .dump());
}
int neverd_pointer_at(neverd_session_t, neverd_va_t address, neverd_va_t *slot,
                      neverd_va_t *target) {
  if (address < DataBase || address >= DataBase + DataSlots * 8)
    return 0;
  const auto first = DataBase + (address - DataBase) / 8 * 8;
  if (slot)
    *slot = first;
  if (target)
    *target = slotTarget((first - DataBase) / 8);
  return 1;
}
const char *neverd_unwind_frame_json(neverd_session_t, neverd_va_t address) {
  // function_0 has a plain frame; function_1 names a personality.
  if (address >= Base && address < Base + 16)
    return copy(Json{
        {"begin", hexAddress(Base)},
        {"end", hexAddress(Base + 16)},
        {"encoding", "dwarf-fde"},
        {"language_data",
         false}}.dump());
  if (address >= Base + 16 && address < Base + 32)
    return copy(Json{
        {"begin", hexAddress(Base + 16)},
        {"end", hexAddress(Base + 32)},
        {"encoding", "dwarf-fde"},
        {"personality", "__gxx_personality_v0"},
        {"language_data",
         true}}.dump());
  return copy("null");
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
  // LLVM-C pages place the definition after a three-line prelude.
  if (std::string(representation) == "llvmc") {
    const std::string prelude =
        "#include <stdint.h>\n"
        "typedef uint64_t _QWORD __attribute__((aligned(1), may_alias));\n"
        "extern int Bar_ctor() __asm__(\"_ZN3BarC1Ev\"); /* Bar::Bar() */\n\n";
    std::vector<std::string> lines = {
        "#include <stdint.h>\n",
        "typedef uint64_t _QWORD __attribute__((aligned(1), may_alias));\n",
        "extern int Bar_ctor() __asm__(\"_ZN3BarC1Ev\"); /* "
        "Bar::Bar() */\n",
        "\n", "/* neverd.entry */\n"};
    for (int i = 0; i < 700; ++i)
      lines.push_back("// code line " + std::to_string(i) + "\n");
    const auto total = lines.size();
    const auto first = std::min(offset, total);
    const auto end = first + std::min(limit, total - first);
    std::size_t byteOffset = 0;
    for (std::size_t i = 0; i < first; ++i)
      byteOffset += lines[i].size();
    std::string text;
    Json rows = Json::array();
    for (auto i = first; i < end; ++i) {
      text += lines[i];
      rows.push_back({{"line", i},
                      {"object_id", "llvmc:line:" + std::to_string(i)},
                      {"kind", "source"},
                      {"mapping_status", "unmapped"},
                      {"addresses", Json::array()}});
    }
    return copy(Json{{"schema_version", 1},
                     {"address", hexAddress(address)},
                     {"representation", representation},
                     {"mapping_status", "library_regions"},
                     {"text", text},
                     {"rows", rows},
                     {"library_regions", Json::array()},
                     {"offset", first},
                     {"byte_offset", byteOffset},
                     {"total_lines", total},
                     {"complete", end == total},
                     {"next_offset", end == total ? Json(nullptr) : Json(end)},
                     {"prelude", {{"lines", 4}, {"end_byte", prelude.size()}}}}
                    .dump());
  }
  // Otherwise this mock represents an older page API that maps Low/Med only;
  // C requests exercise the worker's legacy fallback.
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
// The fixture's function_9 reads as a demangled C++ method.
const char *neverd_demangle(const char *name) {
  if (!name)
    return nullptr;
  return copy(std::string_view(name) == "function_9" ? "Widget::draw()" : name);
}
const char *neverd_switches_json(neverd_session_t, neverd_va_t first, int) {
  // function_8 loads a table of three offsets from the table, in the free
  // end of the read-only data, and dispatches through it.
  constexpr std::uint64_t Entry = Base + 0x80, Table = RodataBase + 0x14;
  Json switches = Json::array();
  if (first <= Entry)
    switches.push_back(
        {{"function", hexAddress(Entry)},
         {"jump", hexAddress(Entry + 6)},
         {"load", hexAddress(Entry + 5)},
         {"table", hexAddress(Table)},
         {"entry_size", 4},
         {"stride", 4},
         {"storage", Json::array({Json::array({hexAddress(Table), 4, 4, 3})})},
         {"form", "table_relative"},
         {"targets",
          Json::array({Json::array({hexAddress(Entry + 8), 0, 0}),
                       Json::array({hexAddress(Entry + 10), 1, 1}),
                       Json::array({hexAddress(Entry + 8), 2, 2})})}});
  return copy(Json{{"switches", switches}, {"next_entry", nullptr}}.dump());
}
const char *neverd_string_encodings_json(void) {
  return copy(Json::array({{{"name", "ascii"},
                            {"spelling", ""},
                            {"unit", 1},
                            {"default", true}},
                           {{"name", "utf-8"},
                            {"spelling", "UTF-8"},
                            {"unit", 1},
                            {"default", true}},
                           {{"name", "utf-16le"},
                            {"spelling", "UTF-16LE"},
                            {"unit", 2},
                            {"default", true}},
                           {{"name", "gbk"},
                            {"spelling", "GBK"},
                            {"unit", 1},
                            {"default", false},
                            {"legacy", true}},
                           {{"name", "big5"},
                            {"spelling", "Big5"},
                            {"unit", 1},
                            {"default", false},
                            {"legacy", true}}})
                  .dump());
}
const char *neverd_strings_ex_json(neverd_session_t, const char *options) {
  const Json parsed =
      options ? Json::parse(options, nullptr, false) : Json::object();
  const auto wanted = [&](const char *encoding) {
    if (!parsed.contains("encodings"))
      return true;
    for (const auto &name : parsed["encodings"])
      if (name == encoding)
        return true;
    return false;
  };
  Json items = Json::array();
  if (wanted("utf-8"))
    items.push_back({{"addr", hexAddress(RodataBase)},
                     {"length", 6},
                     {"chars", 2},
                     {"encoding", "utf-8"},
                     {"value", "\xe4\xb8\xad\xe6\x96\x87"}});
  if (wanted("utf-16le"))
    items.push_back({{"addr", hexAddress(RodataBase + 8)},
                     {"length", 8},
                     {"chars", 4},
                     {"encoding", "utf-16le"},
                     {"value", "Wide"}});
  return copy(items.dump());
}
const char *neverd_decode_text_json(const unsigned char *bytes, int size,
                                    const char *encoding) {
  // ASCII shows printable bytes; UTF-16LE shows printable ASCII units at
  // their first byte and nothing at the second.  Others are unknown.
  const std::string_view name(encoding);
  if ((name != "ascii" && name != "utf-16le") || size < 0)
    return nullptr;
  const auto shown = [](unsigned value) {
    return value >= 0x20 && value < 0x7f
               ? Json(std::string(1, static_cast<char>(value)))
               : Json(nullptr);
  };
  Json cells = Json::array();
  for (int i = 0; i < size; ++i) {
    if (name == "ascii") {
      cells.push_back(shown(bytes[i]));
    } else if (i % 2 == 0 && i + 1 < size) {
      cells.push_back(shown(bytes[i] | bytes[i + 1] << 8));
    } else {
      cells.push_back(i % 2 ? Json("") : Json(nullptr));
    }
  }
  return copy(Json{{"cells", cells}}.dump());
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
                            {"flags", "R-X"}},
                           {{"name", ".data.rel.ro"},
                            {"va", hexAddress(DataBase)},
                            {"size", "0x40"},
                            {"flags", "R--"}},
                           {{"name", ".rodata"},
                            {"va", hexAddress(RodataBase)},
                            {"size", "0x20"},
                            {"flags", "R--"}}})
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
                            {"flags", "R-X"}},
                           {{"name", ".data.rel.ro"},
                            {"segment", ".data.rel.ro"},
                            {"va", hexAddress(DataBase)},
                            {"size", DataSlots * 8},
                            {"file_off", 0x4000},
                            {"file_sz", DataSlots * 8},
                            {"alignment", 8},
                            {"flags", "R--"}},
                           {{"name", ".rodata"},
                            {"segment", ".rodata"},
                            {"va", hexAddress(RodataBase)},
                            {"size", RodataSize},
                            {"file_off", 0x4100},
                            {"file_sz", RodataSize},
                            {"alignment", 16},
                            {"flags", "R--"}}})
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
