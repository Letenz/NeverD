#include "Listing.h"

#include "EngineSymbols.h"

#include "neverd/sdk/NeverDCAPIDisasm.h"
#include "neverd/sdk/NeverDCAPIPersist.h"
#include "neverd/sdk/NeverDCAPIQuery.h"
#include "neverd/sdk/NeverDCAPISession.h"
#include "neverd/sdk/NeverDCAPISigs.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <limits>
#include <list>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace neverd::worker {
namespace {
using DisasmExFunction = const char *(*)(neverd_session_t, neverd_va_t, int,
                                         unsigned);
using CodeRefsFunction = const char *(*)(neverd_session_t, neverd_va_t, int);
using ImportSlotsFunction = const char *(*)(neverd_session_t);
using UnwindFrameFunction = const char *(*)(neverd_session_t, neverd_va_t);
using PointerRefsFunction = const char *(*)(neverd_session_t, neverd_va_t, int);
using DiscoverFunctionsFunction = int (*)(neverd_session_t);
using PointerAtFunction = int (*)(neverd_session_t, neverd_va_t, neverd_va_t *,
                                  neverd_va_t *);
using DataSymbolsFunction = const char *(*)(neverd_session_t);

// Listing layout, in characters after the prefix: name, mnemonic and comment
// columns of a conventional interactive disassembler listing.
constexpr std::size_t NameColumnWidth = 16;
constexpr std::size_t MnemonicWidth = 8;
constexpr std::size_t CommentColumn = 40;
constexpr std::size_t MaxXrefLines = 2;
constexpr std::size_t MaxPageLines = 2000;
constexpr std::size_t MaxOpcodeBytes = 12;
constexpr std::size_t MaxDecodeChunk = 2048;
constexpr std::size_t FirstUnsizedChunk = 64;
constexpr std::size_t DecodeCacheFunctions = 512;
constexpr int IndexStepFunctions = 2048;
constexpr std::size_t MaxStringDisplay = 400;
constexpr std::size_t MaxUnsizedInstructions = 65536;
constexpr std::uint64_t MaxAlignment = 0x1000;
// Classic listings name padding by the alignment of its end, up to this or the
// smallest power of two longer than the padding.
constexpr std::uint64_t ClassicAlignmentLimit = 0x20;
// Automatic string names keep this many characters, the `a` included.
constexpr std::size_t StringNameLength = 15;
constexpr std::size_t MaxOverviewBuckets = 16384;
constexpr int OverviewSamples = 4;
constexpr int StringScanMinimum = 4;
constexpr std::size_t MaxReferencePage = 512;
constexpr int PointerPage = 65536;
constexpr char SeparatorRule[] = "; -------------------------------------------"
                                 "--------------------------------";
constexpr char SubroutineRule[] =
    "; =============== S U B R O U T I N E ======="
    "================================";
constexpr char AttributesLead[] = "; Attributes: ";
constexpr char UnwindOpen[] = "; __unwind {";
constexpr char UnwindClose[] = "; } // starts at ";
constexpr char SegmentRule[] = "; ============================================="
                               "==============================";
// A loader segment's remainder outside every section is mapped in pages.
constexpr std::uint64_t LoadSegmentAlignment = 0x1000;
// The section a classic listing assumes the data segment register holds.
constexpr char DataSectionName[] = ".data";
constexpr char SegmentRegistersLead[] = "es:nothing, ss:nothing, ds:";
constexpr char SegmentRegistersTail[] = ", fs:nothing, gs:nothing";

std::string takeString(const char *value) {
  if (!value)
    return {};
  std::string result(value, strnlen(value, MaxBackendBytes + 1));
  neverd_free_string(value);
  if (result.size() > MaxBackendBytes)
    throw Error("budget_exceeded", "Engine result exceeds the adapter budget");
  return result;
}
Json takeJson(const char *value) {
  const auto text = takeString(value);
  if (text.empty())
    return nullptr;
  try {
    return Json::parse(text);
  } catch (const Json::exception &) {
    throw Error("engine_error", "Engine returned malformed JSON");
  }
}
std::uint64_t jsonAddress(const Json &value) {
  if (!value.is_string())
    return 0;
  const auto &text = value.get_ref<const std::string &>();
  std::uint64_t result = 0;
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    std::from_chars(text.data() + 2, text.data() + text.size(), result, 16);
  return result;
}
std::uint64_t jsonCount(const Json &value) {
  if (value.is_number_unsigned())
    return value.get<std::uint64_t>();
  if (value.is_number_integer())
    return static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, value.get<std::int64_t>()));
  return jsonAddress(value);
}
std::string upperHex(std::uint64_t value, unsigned width = 0) {
  char buffer[24];
  const auto end = std::to_chars(buffer, buffer + sizeof buffer, value, 16).ptr;
  std::string digits(buffer, end);
  std::transform(digits.begin(), digits.end(), digits.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  if (digits.size() < width)
    digits.insert(0, width - digits.size(), '0');
  return digits;
}

enum class Flow : std::uint8_t {
  None,
  Call,
  IndirectCall,
  Jump,
  CondJump,
  IndirectJump,
  Return
};
Flow parseFlow(const std::string &kind) {
  if (kind == "call")
    return Flow::Call;
  if (kind == "icall")
    return Flow::IndirectCall;
  if (kind == "jump")
    return Flow::Jump;
  if (kind == "cjump")
    return Flow::CondJump;
  if (kind == "ijump")
    return Flow::IndirectJump;
  if (kind == "ret")
    return Flow::Return;
  return Flow::None;
}
std::string_view flowName(Flow flow) {
  switch (flow) {
  case Flow::Call:
    return "call";
  case Flow::IndirectCall:
    return "icall";
  case Flow::Jump:
    return "jump";
  case Flow::CondJump:
    return "cjump";
  case Flow::IndirectJump:
    return "ijump";
  case Flow::Return:
    return "ret";
  case Flow::None:
    break;
  }
  return {};
}
bool fallsThrough(Flow flow) {
  return flow != Flow::Jump && flow != Flow::IndirectJump &&
         flow != Flow::Return;
}
bool isBranch(Flow flow) {
  return flow == Flow::Jump || flow == Flow::CondJump;
}

/// IndirectCall and IndirectJump transfer through a relocated pointer slot to
/// the pointer it holds as loaded.
enum class RefKind : std::uint8_t {
  Call,
  Jump,
  CondJump,
  Read,
  Write,
  Offset,
  IndirectCall,
  IndirectJump
};
std::optional<RefKind> parseRefKind(const std::string &kind) {
  if (kind == "call")
    return RefKind::Call;
  if (kind == "jump")
    return RefKind::Jump;
  if (kind == "cjump")
    return RefKind::CondJump;
  if (kind == "read")
    return RefKind::Read;
  if (kind == "write")
    return RefKind::Write;
  if (kind == "offset")
    return RefKind::Offset;
  if (kind == "icall")
    return RefKind::IndirectCall;
  if (kind == "ijump")
    return RefKind::IndirectJump;
  return std::nullopt;
}
std::string_view refKindName(RefKind kind) {
  switch (kind) {
  case RefKind::Call:
    return "call";
  case RefKind::Jump:
    return "jump";
  case RefKind::CondJump:
    return "cjump";
  case RefKind::Read:
    return "read";
  case RefKind::Write:
    return "write";
  case RefKind::Offset:
    return "offset";
  case RefKind::IndirectCall:
    return "icall";
  case RefKind::IndirectJump:
    return "ijump";
  }
  return {};
}
char refKindLetter(RefKind kind) {
  switch (kind) {
  case RefKind::Call:
  case RefKind::IndirectCall:
    return 'p';
  case RefKind::Jump:
  case RefKind::CondJump:
  case RefKind::IndirectJump:
    return 'j';
  case RefKind::Read:
    return 'r';
  case RefKind::Write:
    return 'w';
  case RefKind::Offset:
    return 'o';
  }
  return 'o';
}
bool isCodeRef(RefKind kind) {
  return kind == RefKind::Call || kind == RefKind::Jump ||
         kind == RefKind::CondJump || kind == RefKind::IndirectCall ||
         kind == RefKind::IndirectJump;
}

#define NEVERD_CLASSIC_NAME(Id, Spelling)                                      \
  constexpr std::string_view Id = Spelling;
#define NEVERD_FUNCTION_ATTRIBUTE(Id, Spelling)                                \
  constexpr std::string_view Id = Spelling;
#define NEVERD_FRAME_NAME_PREFIX(Id, Prefix)                                   \
  constexpr std::string_view Id = Prefix;
#define NEVERD_ITEM_NAME_PREFIX(Id, Prefix)                                    \
  constexpr std::string_view Id = Prefix;
#include "ListingVocabulary.def"

struct Region {
  std::uint64_t start = 0, end = 0, initializedEnd = 0, linear = 0;
  std::optional<std::uint64_t> fileOffset;
  std::string name;
  bool read = false, write = false, exec = false;
  std::uint64_t alignment = 1;
};
struct Function {
  std::uint64_t entry = 0, end = 0, size = 0;
  /// Display name; engineName is the engine's own spelling of it.
  std::string name, engineName;
  bool library = false, exported = false;
  /// An executable veneer forwarding to an import (ELF PLT entry, stub).
  bool thunk = false;
};
/// Where an instruction leaves the stack pointer (NEVERD_DISASM_STACK): its
/// value before the instruction plus delta, or base's value plus delta.
struct StackMove {
  std::int64_t delta = 0;
  std::string base;
  bool known = true;
};
struct Instruction {
  std::uint64_t address = 0;
  std::uint32_t size = 0;
  std::string mnemonic, operands, bytes;
  Flow flow = Flow::None;
  std::optional<std::uint64_t> target;
  std::vector<std::pair<std::uint64_t, RefKind>> refs;
  /// Absent when the engine does not state stack moves.
  std::optional<StackMove> stackMove;
  /// The stack pointer's offset from its value at the function's entry
  /// before this instruction, when every path to it agrees.
  std::optional<std::int64_t> stackDelta;
};
/// An unwind frame a listing marks.
struct UnwindMark {
  std::uint64_t begin = 0, end = 0;
  std::string personality;
};
struct DecodedFunction {
  std::vector<Instruction> instructions;
  std::set<std::uint64_t> labels;
  std::uint64_t end = 0;
  std::optional<UnwindMark> unwind;
  /// The frame register a prologue sets up, or empty.
  std::string_view frameRegister;
  /// The stack register, when the stack pointer is tracked.
  std::string_view stackRegister;
  /// The frame's base as an offset from the stack pointer at entry: the frame
  /// register's value, or the entry stack pointer without one.  Variables
  /// lie below it and arguments above the return address and saved frame
  /// pointer.
  std::int64_t frameBase = 0;
  /// Stack variables by offset from the frame's base, with their bytes.
  std::map<std::int64_t, unsigned> frame;
};
struct StringItem {
  std::uint64_t address = 0, length = 0;
  std::string value, name;
};
struct ImportSlot {
  std::string name, module, label;
};
struct Reference {
  std::uint64_t to = 0, from = 0;
  RefKind kind = RefKind::Offset;
};

enum class ItemKind : std::uint8_t {
  Instruction,
  Byte,
  String,
  Align,
  Uninitialized,
  Slot,
  /// A data slot the loader relocated to hold a pointer.
  Pointer
};
struct Item {
  std::uint64_t start = 0, size = 1;
  ItemKind kind = ItemKind::Byte;
  int region = -1, function = -1;
  std::size_t index = 0;
  std::uint64_t alignment = 0;
};

struct Line {
  std::uint64_t item = 0, address = 0;
  std::uint32_t sub = 0;
  AddressClass cls = AddressClass::Unexplored;
  const char *kind = "data";
  StyledText text;
  std::optional<std::uint64_t> target;
  std::string flow;
  int function = -1;
};

Json spansJson(const StyledText &text) {
  Json spans = Json::array();
  for (const auto &span : text.spans()) {
    Json entry = {span.start, span.length, static_cast<int>(span.role)};
    if (span.address)
      entry.push_back(hexAddress(*span.address));
    spans.push_back(std::move(entry));
  }
  return spans;
}

std::string escapeString(const std::string &value, std::size_t limit) {
  std::string result;
  bool quoted = false;
  std::size_t shown = 0;
  for (unsigned char c : value) {
    if (shown++ >= limit) {
      if (quoted)
        result += '\'';
      result += quoted ? ",..." : "...";
      return result;
    }
    if (c >= 0x20 && c < 0x7f && c != '\'') {
      if (!quoted) {
        if (!result.empty())
          result += ',';
        result += '\'';
        quoted = true;
      }
      result += static_cast<char>(c);
    } else {
      if (quoted) {
        result += '\'';
        quoted = false;
      }
      if (!result.empty())
        result += ',';
      result += x86Number(c);
    }
  }
  if (quoted)
    result += '\'';
  if (!result.empty())
    result += ',';
  result += '0';
  return result;
}
} // namespace

struct Listing::Impl {
  neverd_session_t session;
  DisasmExFunction disasmEx = nullptr;
  CodeRefsFunction codeRefs = nullptr;
  ImportSlotsFunction importSlots = nullptr;
  UnwindFrameFunction unwindFrame = nullptr;
  PointerRefsFunction pointerRefs = nullptr;
  DiscoverFunctionsFunction discoverFunctions = nullptr;
  PointerAtFunction pointerAtQuery = nullptr;
  DataSymbolsFunction dataSymbols = nullptr;
  /// Whether the engine's function detector has run for this image.
  bool discovered = false;
  OperandDialect dialect = OperandDialect::Generic;
  bool wide = true, elf = false, macho = false;
  unsigned addressDigits = 16, pointerSize = 8;
  std::uint64_t imageEntry = 0;
  /// Code the loader runs on its own besides the entry (DT_INIT, DT_FINI).
  std::set<std::uint64_t> runtimeEntries;

  bool built = false;
  int builtFunctionCount = -1;
  std::uint64_t generation = 1;
  std::vector<Region> regions;
  std::vector<Function> functions;
  std::unordered_map<std::uint64_t, std::string> dataNames;
  std::unordered_map<std::uint64_t, ImportSlot> slots;
  std::vector<StringItem> strings;
  std::map<std::string, std::uint64_t, std::less<>> listingNames;
  /// Executable import veneers and the import each forwards to.
  std::map<std::uint64_t, std::string> stubImports;
  /// Engine function name -> display name, where they differ.
  std::unordered_map<std::string, std::string> aliases;
  Json functionRowsCache;
  std::uint64_t functionRowsGeneration = 0;
  std::vector<std::uint64_t> namedData;
  std::unordered_map<std::uint64_t, std::uint64_t> alignCache;

  std::unordered_map<std::uint64_t, DecodedFunction> decoded;
  std::list<std::uint64_t> decodedOrder;

  enum class IndexState { Idle, Building, Ready, Unavailable };
  IndexState indexState = IndexState::Idle;
  std::vector<Reference> references; // sorted by (to, from) when Ready
  std::optional<std::uint64_t> indexCursor;
  std::size_t indexDone = 0, indexTotal = 0;
  std::string indexError;

  int opcodeBytes = 0;

  explicit Impl(neverd_session_t s) : session(s) {
    disasmEx = engineSymbol<DisasmExFunction>("neverd_disasm_json_ex");
    codeRefs = engineSymbol<CodeRefsFunction>("neverd_code_refs_json");
    importSlots = engineSymbol<ImportSlotsFunction>("neverd_import_slots_json");
    unwindFrame = engineSymbol<UnwindFrameFunction>("neverd_unwind_frame_json");
    pointerRefs = engineSymbol<PointerRefsFunction>("neverd_pointer_refs_json");
    discoverFunctions = engineSymbol<DiscoverFunctionsFunction>(
        "neverd_session_discover_functions");
    pointerAtQuery = engineSymbol<PointerAtFunction>("neverd_pointer_at");
    dataSymbols = engineSymbol<DataSymbolsFunction>("neverd_data_symbols_json");
  }

  //===--------------------------------------------------------------------===//
  // Indexes
  //===--------------------------------------------------------------------===//

  /// Let the engine add the functions its detector finds without lifting.
  /// The next build sees the longer function list.  A failure leaves the list
  /// as the image states it; the engine reports why.
  void discover() {
    if (discovered)
      return;
    discovered = true;
    if (discoverFunctions)
      (void)discoverFunctions(session);
  }

  void build() {
    const int count = neverd_func_count(session);
    if (built && count == builtFunctionCount)
      return;
    built = true;
    builtFunctionCount = count;
    ++generation;
    const auto arch = takeString(neverd_session_arch_name(session));
    dialect = operandDialect(arch);
    const int bits = neverd_session_bitness(session);
    wide = bits != 32 && bits != 16;
    addressDigits = wide ? 16 : 8;
    pointerSize = wide ? 8 : 4;
    const auto format = takeString(neverd_session_format_name(session));
    elf = format.find("ELF") != std::string::npos;
    macho = format.find("Mach") != std::string::npos;
    buildRegions();
    buildFunctions(count);
    buildNames();
    decoded.clear();
    decodedOrder.clear();
    alignCache.clear();
    applyDisplayNames();
  }

  //===--------------------------------------------------------------------===//
  // Display names
  //===--------------------------------------------------------------------===//

  /// Classic names for functions the engine leaves generic: import thunks
  /// after their import, the image entry point, and main as passed to the C
  /// runtime's start routine.  Engine, symbol and user names always win.
  void applyDisplayNames() {
    const auto rename = [&](Function &function, std::string name) {
      if (!parseDummyName(function.name) || listingNames.contains(name))
        return false;
      function.name = std::move(name);
      return true;
    };
    for (const auto &[stub, import] : stubImports)
      if (auto *function = functionAtEntry(stub))
        if (rename(*function, thunkName(import)))
          function->thunk = true;
    // Linker stubs the import table does not map (ELF .plt.got entries) jump
    // through an import's slot.
    for (std::size_t index = 0; index < functions.size(); ++index) {
      auto &function = functions[index];
      const int region = regionIndex(function.entry);
      if (function.thunk || region < 0 || !parseDummyName(function.name) ||
          !isStubRegion(regions[region].name))
        continue;
      if (const auto import = slotJumpTarget(static_cast<int>(index)))
        if (rename(function, thunkName(*import)))
          function.thunk = true;
    }
    // Loader-run initialization and termination code.
    for (const auto &region : regions)
      if (auto *function = functionAtEntry(region.start))
        if (const auto name = sectionFunctionName(region.name); !name.empty())
          rename(*function, std::string(name));
    const auto entry = neverd_session_entry_addr(session);
    imageEntry = entry;
    runtimeEntries.clear();
    if (const auto rows = takeJson(neverd_entrypoints_json(session));
        rows.is_array())
      for (const auto &row : rows) {
        const auto type = row.value("type", std::string());
#define NEVERD_RUNTIME_ENTRY_TYPE(Type)                                        \
  if (type == Type)                                                            \
    runtimeEntries.insert(jsonAddress(row.value("addr", Json())));
#include "ListingVocabulary.def"
      }
    if (auto *function = functionAtEntry(entry))
      rename(*function, std::string(EntryFunctionName));
    if (!functionNamed(MainFunctionName))
      if (const auto main = startArgumentMain(entry))
        if (auto *function = functionAtEntry(*main))
          rename(*function, std::string(MainFunctionName));
    aliases.clear();
    for (const auto &function : functions)
      if (function.name != function.engineName) {
        listingNames.emplace(function.name, function.entry);
        aliases.emplace(function.engineName, function.name);
      }
  }

  /// The executable name of an import's veneer: ELF PLT entries take a
  /// leading underscore; Mach-O symbols already carry one; other thunks are
  /// jump stubs.
  std::string thunkName(const std::string &import) const {
    if (elf)
      return "_" + import;
    if (macho)
      return import;
    return "j_" + import;
  }

  static bool isStubRegion(std::string_view section) {
    static constexpr std::string_view Prefixes[] = {
#define NEVERD_STUB_SECTION_PREFIX(Prefix) Prefix,
#include "ListingVocabulary.def"
    };
    for (const auto prefix : Prefixes)
      if (section.starts_with(prefix))
        return true;
    return false;
  }

  static std::string_view sectionFunctionName(std::string_view section) {
    static constexpr std::pair<std::string_view, std::string_view> Names[] = {
#define NEVERD_SECTION_FUNCTION_NAME(Section, Name) {Section, Name},
#include "ListingVocabulary.def"
    };
    for (const auto &[name, function] : Names)
      if (section == name)
        return function;
    return {};
  }

  /// The import whose slot a stub's first real instruction jumps through.
  std::optional<std::string> slotJumpTarget(int index) {
    for (const auto &instruction : decode(index).instructions) {
      if (instruction.flow == Flow::None &&
          (instruction.mnemonic == "endbr64" ||
           instruction.mnemonic == "endbr32" || instruction.mnemonic == "nop"))
        continue;
      if (instruction.flow != Flow::IndirectJump &&
          instruction.flow != Flow::Jump)
        return std::nullopt;
      const auto import = calledImport(instruction);
      return import.empty() ? std::nullopt : std::optional<std::string>(import);
    }
    return std::nullopt;
  }

  const Function *functionNamed(std::string_view name) const {
    for (const auto &function : functions)
      if (function.name == name)
        return &function;
    return nullptr;
  }

  /// The import an instruction transfers to: a direct call to a veneer or an
  /// indirect call through an import slot.
  std::string calledImport(const Instruction &instruction) const {
    if (instruction.target) {
      if (auto it = stubImports.find(*instruction.target);
          it != stubImports.end())
        return it->second;
      if (auto it = slots.find(*instruction.target); it != slots.end())
        return it->second.name;
    }
    for (const auto &[address, kind] : instruction.refs)
      if (kind == RefKind::Read)
        if (auto it = slots.find(address); it != slots.end())
          return it->second.name;
    return {};
  }

  /// main is the first argument the image's start routine passes to the C
  /// runtime (`__libc_start_main` and its variants): RDI/EDI on x86-64, the
  /// last pushed code address on x86, X0 from ADRP+ADD on AArch64.
  std::optional<std::uint64_t> startArgumentMain(std::uint64_t entry) {
    const int index = functionIndex(entry);
    if (index < 0 || functions[index].entry != entry)
      return std::nullopt;
    const auto isCode = [&](std::uint64_t address) {
      const int region = regionIndex(address);
      return region >= 0 && regions[region].exec;
    };
    std::optional<std::uint64_t> argument;
    std::optional<std::uint64_t> page; // AArch64 ADRP x0
    for (const auto &instruction : decode(index).instructions) {
      const std::string_view mnemonic = instruction.mnemonic;
      const std::string_view operands = instruction.operands;
      const auto firstOffset = [&]() -> std::optional<std::uint64_t> {
        for (const auto &[address, kind] : instruction.refs)
          if ((kind == RefKind::Offset || kind == RefKind::Read) &&
              isCode(address))
            return address;
        return std::nullopt;
      };
      if (instruction.flow == Flow::Call ||
          instruction.flow == Flow::IndirectCall) {
        if (isStartRoutine(calledImport(instruction)))
          return argument;
        argument.reset();
        continue;
      }
      if ((mnemonic == "lea" || mnemonic == "mov") &&
          (operands.starts_with("rdi,") || operands.starts_with("edi,"))) {
        argument = firstOffset();
      } else if (mnemonic == "push" && !wide) {
        if (const auto value = firstOffset())
          argument = value;
      } else if (mnemonic == "adrp" && operands.starts_with("x0,")) {
        page = parseImmediate(operands.substr(3));
      } else if (mnemonic == "add" && operands.starts_with("x0, x0,") && page) {
        if (const auto offset = parseImmediate(operands.substr(7)))
          argument = *page + *offset;
        page.reset();
      }
    }
    return std::nullopt;
  }

  static bool isStartRoutine(std::string_view import) {
    static constexpr std::string_view Routines[] = {
#define NEVERD_START_ROUTINE(Name) Name,
#include "ListingVocabulary.def"
    };
    for (const auto routine : Routines)
      if (import == routine)
        return true;
    return false;
  }

  /// `#0x1234`, `0x1234` or decimal, as disassembler operands spell them.
  static std::optional<std::uint64_t> parseImmediate(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '#'))
      text.remove_prefix(1);
    int base = 10;
    if (text.starts_with("0x")) {
      text.remove_prefix(2);
      base = 16;
    }
    std::uint64_t value = 0;
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (parsed.ec != std::errc{} || parsed.ptr == text.data())
      return std::nullopt;
    return value;
  }

  void buildRegions() {
    regions.clear();
    struct Range {
      std::uint64_t start = 0, end = 0, initializedEnd = 0, alignment = 1;
      std::string name, flags;
      std::optional<std::uint64_t> fileOffset;
    };
    std::vector<Range> sections, segments;
    if (const auto rows = takeJson(neverd_sections_json(session));
        rows.is_array())
      for (const auto &row : rows) {
        const auto start = jsonAddress(row.value("va", Json()));
        const auto size = jsonCount(row.value("size", Json()));
        if (!start || !size ||
            size > std::numeric_limits<std::uint64_t>::max() - start)
          continue;
        const auto fileSize = jsonCount(row.value("file_sz", Json()));
        Range range{start,
                    start + size,
                    start + std::min(size, fileSize),
                    std::max<std::uint64_t>(
                        1, jsonCount(row.value("alignment", Json()))),
                    row.value("name", std::string()),
                    row.value("flags", std::string()),
                    std::nullopt};
        if (fileSize && row.contains("file_off"))
          range.fileOffset = jsonCount(row["file_off"]);
        sections.push_back(std::move(range));
      }
    if (const auto rows = takeJson(neverd_segments_json(session));
        rows.is_array())
      for (const auto &row : rows) {
        const auto start = jsonAddress(row.value("va", Json()));
        const auto size = jsonCount(row.value("size", Json()));
        if (!size || size > std::numeric_limits<std::uint64_t>::max() - start)
          continue;
        segments.push_back({start, start + size, start + size, 1,
                            row.value("name", std::string()),
                            row.value("flags", std::string())});
      }
    const auto byStart = [](const Range &a, const Range &b) {
      return a.start < b.start;
    };
    std::sort(sections.begin(), sections.end(), byStart);
    std::sort(segments.begin(), segments.end(), byStart);
    std::vector<Range> pieces;
    // Sections name the mapped pieces; an uncovered remainder of a segment
    // keeps the loader-level name.
    for (const auto &segment : segments) {
      std::uint64_t cursor = segment.start;
      const std::string remainder = elf ? std::string("LOAD") : segment.name;
      const std::uint64_t remainderAlignment = elf ? LoadSegmentAlignment : 1;
      for (const auto &section : sections) {
        if (section.end <= cursor || section.start >= segment.end)
          continue;
        const auto start = std::max(section.start, cursor);
        const auto end = std::min(section.end, segment.end);
        if (start > cursor)
          pieces.push_back({cursor, start, start, remainderAlignment, remainder,
                            segment.flags});
        Range piece = section;
        if (piece.fileOffset)
          *piece.fileOffset += start - section.start;
        piece.start = start;
        piece.end = end;
        piece.initializedEnd = std::clamp(section.initializedEnd, start, end);
        if (piece.flags.empty())
          piece.flags = segment.flags;
        pieces.push_back(std::move(piece));
        cursor = end;
      }
      if (cursor < segment.end)
        pieces.push_back({cursor, segment.end, segment.end, remainderAlignment,
                          remainder, segment.flags});
    }
    if (segments.empty())
      pieces = sections;
    std::sort(pieces.begin(), pieces.end(), byStart);
    std::uint64_t linear = 0;
    for (const auto &piece : pieces) {
      if (!regions.empty() && piece.start < regions.back().end)
        continue; // Overlapping loader ranges: the first claim wins.
      Region region;
      region.start = piece.start;
      region.end = piece.end;
      region.initializedEnd = piece.initializedEnd;
      region.name = piece.name.empty() ? std::string("seg") : piece.name;
      region.read = piece.flags.find('R') != std::string::npos;
      region.write = piece.flags.find('W') != std::string::npos;
      region.exec = piece.flags.find('X') != std::string::npos;
      region.alignment = piece.alignment;
      region.fileOffset = piece.fileOffset;
      region.linear = linear;
      linear += region.end - region.start;
      regions.push_back(std::move(region));
    }
  }

  void buildFunctions(int count) {
    functions.clear();
    functions.reserve(std::max(count, 0));
    for (int index = 0; index < count; ++index) {
      Function function;
      function.entry = neverd_func_entry(session, index);
      function.size = static_cast<std::uint64_t>(
          std::max(0, neverd_func_size(session, index)));
      function.name = takeString(neverd_func_name(session, index));
      function.engineName = function.name;
      functions.push_back(std::move(function));
    }
    std::stable_sort(
        functions.begin(), functions.end(),
        [](const Function &a, const Function &b) { return a.entry < b.entry; });
    functions.erase(std::unique(functions.begin(), functions.end(),
                                [](const Function &a, const Function &b) {
                                  return a.entry == b.entry;
                                }),
                    functions.end());
    for (std::size_t i = 0; i < functions.size(); ++i) {
      auto &function = functions[i];
      const int region = regionIndex(function.entry);
      std::uint64_t bound =
          region >= 0 ? regions[region].end : function.entry + 1;
      if (i + 1 < functions.size())
        bound = std::min(bound, functions[i + 1].entry);
      function.end = function.size
                         ? std::min(bound, function.entry + function.size)
                         : bound;
      if (function.end <= function.entry)
        function.end = function.entry + 1;
    }
    // Byte signatures name library code.
    if (const auto matches = takeJson(neverd_sig_matches_json(session));
        matches.is_array())
      for (const auto &match : matches)
        if (auto *function =
                functionAtEntry(jsonAddress(match.value("addr", Json()))))
          function->library = true;
  }

  void buildNames() {
    dataNames.clear();
    slots.clear();
    stubImports.clear();
    strings.clear();
    listingNames.clear();
    namedData.clear();
    // Only names of data matter here; functions are listed already.
    if (const auto rows = takeJson(dataSymbols ? dataSymbols(session)
                                               : neverd_symbols_json(session));
        rows.is_array())
      for (const auto &row : rows) {
        const auto address = jsonAddress(row.value("addr", Json()));
        auto name = row.value("name", std::string());
        if (address && !name.empty() && !functionAtEntry(address))
          dataNames.emplace(address, std::move(name));
      }
    if (const auto rows = takeJson(neverd_imports_json(session));
        rows.is_array())
      for (const auto &row : rows) {
        const auto address = jsonAddress(row.value("iat_addr", Json()));
        auto name = row.value("name", std::string());
        if (!address || name.empty())
          continue;
        for (const auto &stub : row.value("stubs", Json::array()))
          if (const auto at = jsonAddress(stub))
            stubImports.emplace(at, name);
        ImportSlot slot{name, row.value("module", std::string()),
                        elf ? name + "_ptr" : name};
        listingNames.emplace(slot.label, address);
        slots.emplace(address, std::move(slot));
      }
    // Every exact import slot, including data-relocated GOT entries that the
    // import table does not list (ELF GLOB_DAT such as __libc_start_main).
    if (importSlots)
      if (const auto rows = takeJson(importSlots(session)); rows.is_array())
        for (const auto &row : rows) {
          const auto address = jsonAddress(row.value("addr", Json()));
          auto name = row.value("name", std::string());
          if (!address || name.empty() || slots.contains(address) ||
              functionAtEntry(address))
            continue;
          ImportSlot slot{name, std::string(), elf ? name + "_ptr" : name};
          listingNames.emplace(slot.label, address);
          slots.emplace(address, std::move(slot));
        }
    if (const auto rows = takeJson(neverd_exports_json(session));
        rows.is_array())
      for (const auto &row : rows) {
        const auto address = jsonAddress(row.value("addr", Json()));
        if (!address)
          continue;
        if (auto *function = functionAtEntry(address))
          function->exported = true;
        else if (auto name = row.value("name", std::string()); !name.empty())
          dataNames.emplace(address, std::move(name));
      }
    if (const auto rows =
            takeJson(neverd_strings_json(session, StringScanMinimum));
        rows.is_array()) {
      strings.reserve(rows.size());
      for (const auto &row : rows) {
        StringItem item;
        item.address = jsonAddress(row.value("addr", Json()));
        item.length = jsonCount(row.value("length", Json()));
        item.value = row.value("value", std::string());
        if (item.address && item.length)
          strings.push_back(std::move(item));
      }
      std::sort(strings.begin(), strings.end(),
                [](const StringItem &a, const StringItem &b) {
                  return a.address < b.address;
                });
      std::unordered_map<std::string, unsigned> used;
      for (auto &item : strings) {
        if (auto it = dataNames.find(item.address); it != dataNames.end()) {
          item.name = it->second;
          continue;
        }
        const auto base = stringName(item);
        auto &count = used[base];
        item.name = count ? base + "_" + std::to_string(count - 1) : base;
        ++count;
        listingNames.emplace(item.name, item.address);
      }
    }
    for (const auto &[address, name] : dataNames) {
      listingNames.emplace(name, address);
      namedData.push_back(address);
    }
    for (const auto &entry : slots)
      namedData.push_back(entry.first);
    std::sort(namedData.begin(), namedData.end());
  }

  /// Automatic string label: `a` followed by the capitalized words.
  /// The classic automatic name of a string: `a` and its words, each
  /// capitalized, up to StringNameLength characters; `asc_` and the address
  /// when it has no letters or digits.
  static std::string stringName(const StringItem &item) {
    std::string name = "a";
    bool wordStart = true;
    for (unsigned char c : item.value) {
      if (name.size() >= StringNameLength)
        break;
      if (c < 0x80 && std::isalnum(c)) {
        name += wordStart ? static_cast<char>(std::toupper(c))
                          : static_cast<char>(std::tolower(c));
        wordStart = false;
      } else {
        wordStart = true;
      }
    }
    return name.size() == 1 ? "asc_" + upperHex(item.address) : name;
  }

  //===--------------------------------------------------------------------===//
  // Lookup
  //===--------------------------------------------------------------------===//

  int regionIndex(std::uint64_t address) const {
    auto it = std::upper_bound(
        regions.begin(), regions.end(), address,
        [](std::uint64_t value, const Region &r) { return value < r.start; });
    if (it == regions.begin())
      return -1;
    --it;
    return address < it->end ? static_cast<int>(it - regions.begin()) : -1;
  }
  Function *functionAtEntry(std::uint64_t address) {
    auto it = std::lower_bound(
        functions.begin(), functions.end(), address,
        [](const Function &f, std::uint64_t value) { return f.entry < value; });
    return it != functions.end() && it->entry == address ? &*it : nullptr;
  }
  /// Index of the function whose decoded extent contains \p address.
  int functionIndex(std::uint64_t address) {
    auto it = std::upper_bound(
        functions.begin(), functions.end(), address,
        [](std::uint64_t value, const Function &f) { return value < f.entry; });
    if (it == functions.begin())
      return -1;
    --it;
    const int index = static_cast<int>(it - functions.begin());
    if (address >= it->end)
      return -1;
    if (!it->size && address != it->entry && address >= decode(index).end)
      return -1;
    return index;
  }
  /// Function extent without decoding, for coarse classification.
  int functionIndexCoarse(std::uint64_t address) const {
    auto it = std::upper_bound(
        functions.begin(), functions.end(), address,
        [](std::uint64_t value, const Function &f) { return value < f.entry; });
    if (it == functions.begin())
      return -1;
    --it;
    return address < it->end ? static_cast<int>(it - functions.begin()) : -1;
  }
  const StringItem *stringAt(std::uint64_t address) const {
    auto it = std::upper_bound(strings.begin(), strings.end(), address,
                               [](std::uint64_t value, const StringItem &s) {
                                 return value < s.address;
                               });
    if (it == strings.begin())
      return nullptr;
    --it;
    return address <= it->address + it->length ? &*it : nullptr;
  }

  std::vector<Instruction> disassemble(std::uint64_t address,
                                       std::size_t count) {
    const char *raw =
        disasmEx
            ? disasmEx(session, address, static_cast<int>(count),
                       NEVERD_DISASM_FLOW | NEVERD_DISASM_STACK)
            : neverd_disasm_json(session, address, static_cast<int>(count));
    const auto rows = takeJson(raw);
    std::vector<Instruction> result;
    if (!rows.is_array())
      return result;
    result.reserve(rows.size());
    for (const auto &row : rows) {
      Instruction instruction;
      instruction.address = jsonAddress(row.value("addr", Json()));
      instruction.size =
          static_cast<std::uint32_t>(jsonCount(row.value("size", Json())));
      instruction.mnemonic = row.value("mnemonic", std::string());
      instruction.operands = row.value("op_str", std::string());
      instruction.bytes = row.value("bytes", std::string());
      instruction.flow = parseFlow(row.value("flow", std::string()));
      if (row.contains("target"))
        instruction.target = jsonAddress(row["target"]);
      if (const auto refs = row.value("refs", Json()); refs.is_array())
        for (const auto &ref : refs)
          if (auto kind = parseRefKind(ref.value("kind", std::string())))
            instruction.refs.push_back(
                {jsonAddress(ref.value("to", Json())), *kind});
      if (const auto sp = row.find("sp"); sp != row.end()) {
        StackMove move;
        move.known = sp->is_number_integer();
        if (move.known) {
          move.delta = sp->get<std::int64_t>();
          move.base = row.value("sp_base", std::string());
        }
        instruction.stackMove = std::move(move);
      }
      if (!instruction.size)
        break;
      result.push_back(std::move(instruction));
    }
    return result;
  }

  /// The stack variables of a function: offsets it addresses through the
  /// frame register its prologue sets up (`push rbp; mov rbp, rsp`, after an
  /// optional endbr), or through the stack register where the stack pointer
  /// is tracked.  An offset starts a variable as wide as its widest access (a
  /// byte when only its address is taken), unless it falls inside the
  /// variable below it.
  void buildFrame(DecodedFunction &body) const {
    const auto &instructions = body.instructions;
    std::size_t first = 0;
    while (first < instructions.size() &&
           (instructions[first].mnemonic == "endbr64" ||
            instructions[first].mnemonic == "endbr32"))
      ++first;
    if (first + 1 < instructions.size() &&
        instructions[first].mnemonic == "push" &&
        instructions[first + 1].mnemonic == "mov") {
#define NEVERD_X86_FRAME_REGISTER(Name, PrologueSource)                        \
  if (instructions[first].operands == Name &&                                  \
      instructions[first + 1].operands ==                                      \
          std::string(Name) + ", " + PrologueSource)                           \
    body.frameRegister = Name;
#include "ListingVocabulary.def"
    }
    // The pushed frame pointer sits between the return address and the
    // frame register's value.
    if (!body.frameRegister.empty())
      body.frameBase = -static_cast<std::int64_t>(pointerSize);
    trackStack(body);
    if (body.frameRegister.empty() && body.stackRegister.empty())
      return;
    // Offset -> bytes of its widest access, or zero.
    std::map<std::int64_t, unsigned> accesses;
    for (const auto &instruction : instructions) {
      std::optional<FrameAccess> access;
      if (!body.frameRegister.empty())
        access = x86FrameAccess(instruction.operands, body.frameRegister);
      if (const auto depth = stackDepth(body, instruction); !access && depth)
        if ((access = x86FrameAccess(instruction.operands, body.stackRegister)))
          access->offset -= *depth;
      // The saved frame pointer and return address are not variables.
      if (!access ||
          (access->offset >= 0 && access->offset < argumentBase(body)))
        continue;
      auto &size = accesses[access->offset];
      size = std::max(size, access->size);
    }
    std::int64_t covered = std::numeric_limits<std::int64_t>::min();
    for (const auto &[offset, size] : accesses) {
      if (offset < covered)
        continue;
      const unsigned bytes = std::max(1u, size);
      body.frame.emplace(offset, bytes);
      covered = offset + static_cast<std::int64_t>(bytes);
    }
  }

  /// The stack pointer's offset from its entry value before each instruction,
  /// carried from the entry along fallthrough and direct branches inside the
  /// body with the engine's stack moves.  An instruction that paths reach
  /// with different offsets, or after a move the engine cannot state, gets
  /// none, and neither do instructions only an indirect jump reaches.  Only
  /// code with 8-byte pointers is tracked: there a callee returns with the
  /// stack pointer its caller had, while a 32-bit callee may also pop its
  /// arguments.
  void trackStack(DecodedFunction &body) const {
    auto &instructions = body.instructions;
    if (!wide || instructions.empty() ||
        std::any_of(instructions.begin(), instructions.end(),
                    [](const Instruction &i) { return !i.stackMove; }))
      return;
#define NEVERD_X86_STACK_REGISTER(Name, PointerBytes)                          \
  if (PointerBytes == pointerSize)                                             \
    body.stackRegister = Name;
#include "ListingVocabulary.def"
    if (body.stackRegister.empty())
      return;
    enum class State : std::uint8_t { Unseen, Known, Conflicting };
    std::vector<State> state(instructions.size(), State::Unseen);
    std::vector<std::int64_t> delta(instructions.size(), 0);
    std::vector<std::size_t> work;
    const auto reach = [&](std::size_t index,
                           std::optional<std::int64_t> value) {
      if (state[index] == State::Conflicting ||
          (state[index] == State::Known && value && delta[index] == *value))
        return;
      state[index] = state[index] == State::Unseen && value
                         ? State::Known
                         : State::Conflicting;
      if (value)
        delta[index] = *value;
      work.push_back(index);
    };
    const auto indexOf =
        [&](std::uint64_t address) -> std::optional<std::size_t> {
      const auto it =
          std::lower_bound(instructions.begin(), instructions.end(), address,
                           [](const Instruction &i, std::uint64_t value) {
                             return i.address < value;
                           });
      if (it == instructions.end() || it->address != address)
        return std::nullopt;
      return static_cast<std::size_t>(it - instructions.begin());
    };
    reach(0, 0);
    while (!work.empty()) {
      const auto index = work.back();
      work.pop_back();
      const Instruction &instruction = instructions[index];
      const StackMove &move = *instruction.stackMove;
      std::optional<std::int64_t> after;
      if (move.known && move.base.empty() && state[index] == State::Known)
        after = delta[index] + move.delta;
      // Relative to the frame register, wherever the stack pointer was.
      else if (move.known && !move.base.empty() &&
               move.base == body.frameRegister)
        after = body.frameBase + move.delta;
      const auto next = [&] {
        if (index + 1 < instructions.size())
          reach(index + 1, after);
      };
      const auto branch = [&] {
        if (instruction.target)
          if (const auto to = indexOf(*instruction.target))
            reach(*to, after);
      };
      switch (instruction.flow) {
      case Flow::Return:
      case Flow::IndirectJump:
        break;
      case Flow::Jump:
        branch();
        break;
      case Flow::CondJump:
        branch();
        next();
        break;
      default:
        next();
        break;
      }
    }
    for (std::size_t index = 0; index < instructions.size(); ++index)
      if (state[index] == State::Known)
        instructions[index].stackDelta = delta[index];
  }

  /// The distance from the stack pointer down from the frame's base before
  /// \p instruction, when it is known and the instruction's operands address
  /// the stack as it is before the instruction: one that does not move the
  /// stack pointer.
  std::optional<std::int64_t> stackDepth(const DecodedFunction &body,
                                         const Instruction &instruction) const {
    if (body.stackRegister.empty() || !instruction.stackDelta ||
        !instruction.stackMove || !instruction.stackMove->known ||
        !instruction.stackMove->base.empty() ||
        instruction.stackMove->delta != 0)
      return std::nullopt;
    const std::int64_t depth = body.frameBase - *instruction.stackDelta;
    if (depth < 0)
      return std::nullopt;
    return depth;
  }

  /// Frame offset of the first stack argument: above the return address and
  /// the saved frame pointer, if any.
  std::int64_t argumentBase(const DecodedFunction &body) const {
    return static_cast<std::int64_t>(pointerSize) - body.frameBase;
  }

  std::string frameName(const DecodedFunction &body,
                        std::int64_t offset) const {
    return offset < 0 ? std::string(LocalPrefix) + upperHex(-offset)
                      : std::string(ArgumentPrefix) +
                            upperHex(offset - argumentBase(body));
  }

  /// The frame variable that \p offset falls in.
  std::optional<FrameSlot> frameSlot(const DecodedFunction &body,
                                     std::int64_t offset) const {
    auto next = body.frame.upper_bound(offset);
    if (next == body.frame.begin())
      return std::nullopt;
    const auto &[start, size] = *std::prev(next);
    if (offset >= start + static_cast<std::int64_t>(size))
      return std::nullopt;
    return FrameSlot{frameName(body, start), offset - start, size};
  }

  const DecodedFunction &decode(int index) {
    auto &function = functions[index];
    if (auto it = decoded.find(function.entry); it != decoded.end())
      return it->second;
    DecodedFunction body;
    std::uint64_t cursor = function.entry;
    std::uint64_t furthestTarget = 0;
    bool stop = false;
    // An unsized body usually ends within a few dozen instructions; growing
    // chunks keep the engine from lifting far past its terminator.
    std::size_t chunkSize = function.size ? MaxDecodeChunk : FirstUnsizedChunk;
    while (!stop && cursor < function.end &&
           body.instructions.size() < MaxUnsizedInstructions) {
      const std::size_t requested = chunkSize;
      auto chunk = disassemble(cursor, requested);
      chunkSize = std::min(MaxDecodeChunk, chunkSize * 2);
      if (chunk.empty())
        break;
      for (auto &instruction : chunk) {
        if (instruction.address != cursor ||
            instruction.address >= function.end) {
          stop = true;
          break;
        }
        if (instruction.target && *instruction.target > instruction.address &&
            *instruction.target < function.end && isBranch(instruction.flow))
          furthestTarget = std::max(furthestTarget, *instruction.target);
        cursor = instruction.address + instruction.size;
        const Flow flow = instruction.flow;
        body.instructions.push_back(std::move(instruction));
        // An unsized function's body ends at a terminator that no earlier
        // branch jumps beyond.
        if (!function.size && !fallsThrough(flow) && cursor > furthestTarget) {
          stop = true;
          break;
        }
      }
      if (chunk.size() < requested)
        break;
    }
    body.end = body.instructions.empty() ? function.entry + 1 : cursor;
    if (!function.size)
      function.end =
          std::max(function.entry + 1, std::min(function.end, body.end));
    for (const auto &instruction : body.instructions)
      if (instruction.target && isBranch(instruction.flow) &&
          *instruction.target > function.entry &&
          *instruction.target < body.end)
        body.labels.insert(*instruction.target);
    // Branches from other functions into this body need labels too.
    if (indexState == IndexState::Ready) {
      auto it = std::lower_bound(
          references.begin(), references.end(), function.entry + 1,
          [](const Reference &r, std::uint64_t value) { return r.to < value; });
      for (; it != references.end() && it->to < body.end; ++it)
        if (it->kind == RefKind::Jump || it->kind == RefKind::CondJump ||
            it->kind == RefKind::IndirectJump)
          body.labels.insert(it->to);
    }
    if (dialect == OperandDialect::X86)
      buildFrame(body);
    body.unwind = unwindAt(function.entry);
    // Only real instruction boundaries can carry labels.
    for (auto it = body.labels.begin(); it != body.labels.end();) {
      const auto at =
          std::lower_bound(body.instructions.begin(), body.instructions.end(),
                           *it, [](const Instruction &i, std::uint64_t value) {
                             return i.address < value;
                           });
      it = at != body.instructions.end() && at->address == *it
               ? std::next(it)
               : body.labels.erase(it);
    }
    decodedOrder.push_front(function.entry);
    if (decodedOrder.size() > DecodeCacheFunctions) {
      decoded.erase(decodedOrder.back());
      decodedOrder.pop_back();
    }
    return decoded.emplace(function.entry, std::move(body)).first->second;
  }

  /// End of the function preceding \p address, or the region start.
  std::uint64_t previousCodeEnd(std::uint64_t address, const Region &region) {
    auto it = std::upper_bound(
        functions.begin(), functions.end(), address,
        [](std::uint64_t value, const Function &f) { return value < f.entry; });
    if (it == functions.begin())
      return region.start;
    const int index = static_cast<int>((it - 1) - functions.begin());
    if (!functions[index].size)
      decode(index);
    return std::clamp(functions[index].end, region.start, region.end);
  }
  std::uint64_t nextFunctionStart(std::uint64_t address,
                                  const Region &region) const {
    auto it = std::upper_bound(
        functions.begin(), functions.end(), address,
        [](std::uint64_t value, const Function &f) { return value < f.entry; });
    return it != functions.end() ? std::min(it->entry, region.end) : region.end;
  }

  /// Alignment of a gap filled only by padding, or zero.
  std::uint64_t paddingAlignment(std::uint64_t start, std::uint64_t end) {
    if (end <= start)
      return 0;
    if (auto it = alignCache.find(start); it != alignCache.end())
      return it->second;
    std::uint64_t alignment = 0;
    const std::uint64_t length = end - start;
    std::uint64_t limit = ClassicAlignmentLimit;
    while (limit <= length && limit < MaxAlignment)
      limit *= 2;
    for (std::uint64_t candidate = limit; candidate >= 2; candidate /= 2)
      if (end % candidate == 0 && length < candidate) {
        alignment = candidate;
        break;
      }
    if (alignment) {
      std::vector<unsigned char> bytes(length);
      const int read = neverd_read_bytes(session, start, bytes.data(),
                                         static_cast<int>(length));
      bool uniform = read == static_cast<int>(length);
      for (std::uint64_t i = 1; uniform && i < length; ++i)
        uniform = bytes[i] == bytes[0];
      const bool paddingByte =
          uniform && (bytes[0] == 0xcc || bytes[0] == 0x90 || bytes[0] == 0);
      if (!paddingByte) {
        // Multi-byte no-op sequences pad between functions as well.
        std::uint64_t cursor = start;
        for (const auto &instruction : disassemble(start, 64)) {
          if (instruction.address != cursor ||
              !isPaddingMnemonic(instruction.mnemonic))
            break;
          cursor += instruction.size;
          if (cursor >= end)
            break;
        }
        if (cursor != end)
          alignment = 0;
      }
    }
    alignCache.emplace(start, alignment);
    return alignment;
  }

  std::optional<Item> itemAt(std::uint64_t address) {
    const int r = regionIndex(address);
    if (r < 0)
      return std::nullopt;
    const Region &region = regions[r];
    Item item;
    item.region = r;
    item.start = address;
    if (address >= region.initializedEnd) {
      std::uint64_t start = region.initializedEnd, end = region.end;
      auto next = std::upper_bound(namedData.begin(), namedData.end(), address);
      if (next != namedData.end() && *next < end)
        end = *next;
      if (next != namedData.begin() && *(next - 1) >= start)
        start = *(next - 1);
      item.kind = ItemKind::Uninitialized;
      item.start = start;
      item.size = end - start;
      return item;
    }
    if (region.exec) {
      if (const int f = functionIndex(address); f >= 0) {
        const auto &body = decode(f);
        item.function = f;
        auto it = std::upper_bound(
            body.instructions.begin(), body.instructions.end(), address,
            [](std::uint64_t value, const Instruction &i) {
              return value < i.address;
            });
        if (it != body.instructions.begin()) {
          --it;
          if (address < it->address + it->size) {
            item.kind = ItemKind::Instruction;
            item.start = it->address;
            item.size = it->size;
            item.index =
                static_cast<std::size_t>(it - body.instructions.begin());
            return item;
          }
        }
        return item; // An undecoded byte inside a function.
      }
      const std::uint64_t gapStart = previousCodeEnd(address, region);
      const std::uint64_t gapEnd =
          std::min(nextFunctionStart(address, region), region.initializedEnd);
      if (gapStart <= address && address < gapEnd)
        if (const auto alignment = paddingAlignment(gapStart, gapEnd)) {
          item.kind = ItemKind::Align;
          item.start = gapStart;
          item.size = gapEnd - gapStart;
          item.alignment = alignment;
        }
      return item;
    }
    for (std::uint64_t back = 0; back < pointerSize && back <= address; ++back)
      if (slots.contains(address - back) && address - back >= region.start) {
        item.kind = ItemKind::Slot;
        item.start = address - back;
        item.size =
            std::min<std::uint64_t>(pointerSize, region.end - item.start);
        return item;
      }
    if (const auto pointer = pointerAt(address);
        pointer && pointer->slot >= region.start &&
        pointer->slot + pointerSize <= region.end) {
      item.kind = ItemKind::Pointer;
      item.start = pointer->slot;
      item.size = pointerSize;
      return item;
    }
    if (const auto *string = stringAt(address);
        string && string->address >= region.start &&
        string->address + string->length < region.end) {
      item.kind = ItemKind::String;
      item.start = string->address;
      item.size = string->length + 1;
      item.index = static_cast<std::size_t>(string - strings.data());
    }
    return item;
  }
  std::optional<Item> nextItem(const Item &item) {
    const auto end = item.start + item.size;
    if (end < regions[item.region].end)
      return itemAt(end);
    for (std::size_t r = item.region + 1; r < regions.size(); ++r)
      if (auto next = itemAt(regions[r].start))
        return next;
    return std::nullopt;
  }
  std::optional<Item> previousItem(const Item &item) {
    if (item.start > regions[item.region].start)
      return itemAt(item.start - 1);
    for (int r = item.region - 1; r >= 0; --r)
      if (auto previous = itemAt(regions[r].end - 1))
        return previous;
    return std::nullopt;
  }

  //===--------------------------------------------------------------------===//
  // Names
  //===--------------------------------------------------------------------===//

  LocationName nameOf(std::uint64_t address, NameUse use,
                      std::string_view sizeKeyword) {
    if (auto *function = functionAtEntry(address))
      return {function->name,
              function->library                ? ListingRole::LibraryName
              : parseDummyName(function->name) ? ListingRole::DummyCodeName
                                               : ListingRole::CodeName,
              address};
    if (auto slot = slots.find(address); slot != slots.end())
      return {slot->second.label, ListingRole::ImportName, address};
    if (auto data = dataNames.find(address); data != dataNames.end())
      return {data->second, ListingRole::DataName, address};
    if (const auto *string = stringAt(address);
        string && string->address == address)
      return {string->name, ListingRole::DummyDataName, address};
    if (use != NameUse::Transfer && isPointerSlot(address))
      return {std::string(PointerNamePrefix) + upperHex(address),
              ListingRole::DummyDataName, address};
    const int region = regionIndex(address);
    if (region < 0)
      return {};
    if (regions[region].exec) {
      const int f = functionIndex(address);
      if (use == NameUse::Transfer || f >= 0)
        return {(f < 0                   ? "unk_"
                 : returnsAt(f, address) ? "locret_"
                                         : "loc_") +
                    upperHex(address),
                ListingRole::DummyCodeName, address};
    }
    std::string prefix = "unk_";
    if (use == NameUse::Slot)
      prefix = PointerNamePrefix;
    else if (use == NameUse::Data)
      if (const auto sized = dataNamePrefix(sizeKeyword); !sized.empty())
        prefix = std::string(sized);
    return {prefix + upperHex(address), ListingRole::DummyDataName, address};
  }

  static bool isMarkedUnwindEncoding(std::string_view encoding) {
#define NEVERD_MARKED_UNWIND_ENCODING(Spelling)                                \
  if (encoding == Spelling)                                                    \
    return true;
#include "ListingVocabulary.def"
    return false;
  }

  /// A data slot the loader relocated to hold a pointer.
  struct PointerSlot {
    std::uint64_t slot = 0, target = 0;
  };
  std::optional<PointerSlot> pointerAt(std::uint64_t address) const {
    neverd_va_t slot = 0, target = 0;
    if (!pointerAtQuery || !pointerAtQuery(session, address, &slot, &target))
      return std::nullopt;
    return PointerSlot{slot, target};
  }
  bool isPointerSlot(std::uint64_t address) const {
    const auto pointer = pointerAt(address);
    return pointer && pointer->slot == address;
  }

  /// The marked unwind frame that covers \p address: every DWARF frame, and
  /// a frame of another format that names a handler.
  std::optional<UnwindMark> unwindAt(std::uint64_t address) {
    if (!unwindFrame)
      return std::nullopt;
    const auto frame = takeJson(unwindFrame(session, address));
    if (!frame.is_object())
      return std::nullopt;
    UnwindMark mark;
    mark.begin = jsonAddress(frame.value("begin", Json()));
    mark.end = jsonAddress(frame.value("end", Json()));
    mark.personality = frame.value("personality", std::string());
    if (mark.end <= mark.begin ||
        (!isMarkedUnwindEncoding(frame.value("encoding", std::string())) &&
         mark.personality.empty()))
      return std::nullopt;
    return mark;
  }

  /// Whether the instruction at \p address in function \p f returns.
  bool returnsAt(int f, std::uint64_t address) {
    const auto &instructions = decode(f).instructions;
    const auto it =
        std::lower_bound(instructions.begin(), instructions.end(), address,
                         [](const Instruction &i, std::uint64_t value) {
                           return i.address < value;
                         });
    return it != instructions.end() && it->address == address &&
           it->flow == Flow::Return;
  }

  std::string locationText(std::uint64_t address) {
    if (const int f = functionIndexCoarse(address); f >= 0) {
      const auto &function = functions[f];
      if (address == function.entry)
        return function.name;
      return function.name + "+" + upperHex(address - function.entry);
    }
    const int region = regionIndex(address);
    if (region < 0)
      return "?:" + upperHex(address, addressDigits);
    // A named or referenced item reads by its name, like `.got:off_AC5AC0`.
    if (dataNames.contains(address) || slots.contains(address) ||
        (isPointerSlot(address) &&
         referencesTo(address).first != referencesTo(address).second))
      return regions[region].name + ":" +
             nameOf(address, NameUse::Data, {}).text;
    return regions[region].name + ":" + upperHex(address, addressDigits);
  }

  std::pair<std::vector<Reference>::const_iterator,
            std::vector<Reference>::const_iterator>
  referencesTo(std::uint64_t address) const {
    if (indexState != IndexState::Ready)
      return {references.end(), references.end()};
    const auto lower = std::lower_bound(
        references.begin(), references.end(), address,
        [](const Reference &r, std::uint64_t value) { return r.to < value; });
    auto upper = lower;
    while (upper != references.end() && upper->to == address)
      ++upper;
    return {lower, upper};
  }

  //===--------------------------------------------------------------------===//
  // Line formatting
  //===--------------------------------------------------------------------===//

  AddressClass classOf(const Item &item) const {
    switch (item.kind) {
    case ItemKind::Instruction:
      return functions[item.function].thunk     ? AddressClass::External
             : functions[item.function].library ? AddressClass::LibraryFunction
                                                : AddressClass::RegularFunction;
    case ItemKind::Byte:
      return AddressClass::Unexplored;
    case ItemKind::Slot:
      return AddressClass::External;
    case ItemKind::String:
    case ItemKind::Align:
    case ItemKind::Uninitialized:
    case ItemKind::Pointer:
      return AddressClass::Data;
    }
    return AddressClass::Unexplored;
  }

  std::string prefixOf(std::uint64_t address, int region) const {
    return regions[region].name + ":" + upperHex(address, addressDigits);
  }

  StyledText lead(std::size_t width) const {
    StyledText text;
    if (width)
      text.append(std::string(width, ' '), ListingRole::Plain);
    return text;
  }

  void appendComment(StyledText &text, std::string_view comment,
                     ListingRole role, std::size_t base) {
    if (comment.empty())
      return;
    text.padTo(base + CommentColumn);
    text.append("; ", role);
    text.append(comment, role);
  }

  /// "CODE XREF: sub_X+1A↑j" lines for references to \p address: code
  /// references first, then data references, each group under its heading.
  std::vector<StyledText> xrefComments(std::uint64_t address) {
    std::vector<StyledText> result;
    auto [begin, end] = referencesTo(address);
    if (begin == end)
      return result;
    const auto total = static_cast<std::size_t>(end - begin);
    for (const bool code : {true, false}) {
      bool heading = true;
      for (auto it = begin; it != end && result.size() < MaxXrefLines; ++it) {
        if (isCodeRef(it->kind) != code)
          continue;
        StyledText line;
        if (heading)
          line.append(code ? "CODE XREF: " : "DATA XREF: ", ListingRole::Xref);
        heading = false;
        line.append(locationText(it->from), ListingRole::Xref, it->from);
        line.append(it->from < address ? "↑" : "↓", ListingRole::Xref);
        line.append(std::string(1, refKindLetter(it->kind)), ListingRole::Xref);
        result.push_back(std::move(line));
      }
    }
    if (total > result.size())
      result.back().append(" ...", ListingRole::Xref);
    return result;
  }

  void addLine(std::vector<Line> &out, const Item &item, std::uint64_t address,
               const char *kind, StyledText text,
               std::optional<std::uint64_t> target = std::nullopt) {
    Line line;
    line.item = item.start;
    line.address = address;
    line.sub = 0;
    line.cls = classOf(item);
    line.function = item.function;
    line.kind = kind;
    line.text = std::move(text);
    line.target = target;
    out.push_back(std::move(line));
  }

  /// Name-column text followed by xref comments on continuation lines.
  void addNamedLine(std::vector<Line> &out, const Item &item, std::size_t base,
                    StyledText head, std::uint64_t address, const char *kind) {
    auto xrefs = xrefComments(address);
    if (!xrefs.empty()) {
      head.padTo(base + CommentColumn);
      head.append("; ", ListingRole::Xref);
      head.append(xrefs.front());
    }
    addLine(out, item, item.start, kind, std::move(head));
    for (std::size_t i = 1; i < xrefs.size(); ++i) {
      StyledText more = lead(base);
      more.padTo(base + CommentColumn);
      more.append("; ", ListingRole::Xref);
      more.append(xrefs[i]);
      addLine(out, item, item.start, "xref", std::move(more));
    }
  }

  std::string segmentDirectiveName(const Region &region) const {
    std::string name = region.name;
    for (auto &c : name)
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
        c = '_';
    return name;
  }

  void segmentHeader(std::vector<Line> &out, const Item &item,
                     std::size_t base) {
    const Region &region = regions[item.region];
    addLine(out, item, item.start, "separator", [&] {
      auto t = lead(base ? base - NameColumnWidth : 0);
      t.append(SegmentRule, ListingRole::Separator);
      return t;
    }());
    addLine(out, item, item.start, "blank", {});
    const bool uninitialized = region.initializedEnd == region.start;
    StyledText type;
    type.append("; Segment type: ", ListingRole::AutoComment);
    type.append(region.exec       ? "Pure code"
                : uninitialized   ? "Uninitialized"
                : slotsIn(region) ? "Externs"
                                  : "Pure data",
                ListingRole::AutoComment);
    addLine(out, item, item.start, "comment", std::move(type));
    std::string permissions;
    for (const auto &[present, word] :
         {std::pair{region.read, "Read"}, std::pair{region.write, "Write"},
          std::pair{region.exec, "Execute"}})
      if (present)
        permissions += (permissions.empty() ? "" : "/") + std::string(word);
    StyledText access;
    access.append("; Segment permissions: ", ListingRole::AutoComment);
    access.append(permissions.empty() ? "None" : permissions,
                  ListingRole::AutoComment);
    addLine(out, item, item.start, "comment", std::move(access));
    StyledText directive;
    directive.append(segmentDirectiveName(region), ListingRole::SegmentName);
    directive.padTo(NameColumnWidth);
    directive.append("segment ", ListingRole::Directive);
    std::string_view alignment;
#define NEVERD_SEGMENT_ALIGNMENT(Bytes, Spelling)                              \
  if (alignment.empty() && region.alignment >= Bytes)                          \
    alignment = Spelling;
#include "ListingVocabulary.def"
    directive.append(std::string(alignment), ListingRole::Directive);
    directive.append(" public '", ListingRole::Directive);
    directive.append(region.exec     ? "CODE"
                     : uninitialized ? "BSS"
                     : region.write  ? "DATA"
                                     : "CONST",
                     ListingRole::Directive);
    directive.append(wide ? "' use64" : "' use32", ListingRole::Directive);
    addLine(out, item, item.start, "directive", std::move(directive));
    if (dialect != OperandDialect::X86)
      return;
    // The segment registers the code assumes; data states only its origin.
    const auto assume = [&](std::string text) {
      StyledText line = lead(base);
      line.append("assume ", ListingRole::Directive);
      line.append(text, ListingRole::Directive);
      addLine(out, item, item.start, "directive", std::move(line));
    };
    assume("cs:" + segmentDirectiveName(region));
    if (region.start) {
      StyledText origin = lead(base);
      origin.append(";org ", ListingRole::AutoComment);
      origin.append(x86Number(region.start), ListingRole::AutoComment);
      addLine(out, item, item.start, "comment", std::move(origin));
    }
    if (region.exec)
      assume(SegmentRegistersLead + dataSegmentName() + SegmentRegistersTail);
  }

  /// The data segment register's assumed segment: the data section's, or
  /// nothing without one.
  std::string dataSegmentName() const {
    for (const auto &region : regions)
      if (region.name == DataSectionName)
        return segmentDirectiveName(region);
    return "nothing";
  }
  bool slotsIn(const Region &region) const {
    for (const auto &entry : slots)
      if (entry.first >= region.start && entry.first < region.end)
        return true;
    return false;
  }

  /// Opcode bytes column of \p width characters, or blanks.
  StyledText opcodeColumn(const Instruction &instruction,
                          std::size_t width) const {
    StyledText column;
    if (!width)
      return column;
    std::string spelled;
    const std::size_t count = instruction.bytes.size() / 2;
    const std::size_t shown =
        std::min<std::size_t>(count, static_cast<std::size_t>(opcodeBytes));
    for (std::size_t i = 0; i < shown; ++i) {
      spelled += static_cast<char>(std::toupper(instruction.bytes[2 * i]));
      spelled += static_cast<char>(std::toupper(instruction.bytes[2 * i + 1]));
      spelled += i + 1 == shown && count > shown ? '+' : ' ';
    }
    column.append(spelled, ListingRole::OpcodeBytes);
    column.padTo(width);
    return column;
  }

  StyledText instructionText(const Instruction &instruction, std::size_t base,
                             StyledText text = {},
                             const DecodedFunction *body = nullptr) {
    text.padTo(base);
    const auto classic = classicInstruction(
        dialect, instruction.mnemonic, instruction.operands, instruction.bytes);
    // Control transfers get their own role, like a source editor's control
    // keywords.
    text.append(classic.mnemonic, instruction.flow == Flow::None
                                      ? ListingRole::Mnemonic
                                      : ListingRole::FlowMnemonic);
    OperandFacts facts;
    facts.dialect = dialect;
    // Without its prefixes (`rep movsb`).
    const auto space = classic.mnemonic.rfind(' ');
    facts.mnemonic = std::string_view(classic.mnemonic)
                         .substr(space == std::string::npos ? 0 : space + 1);
    facts.address = instruction.address;
    facts.size = instruction.size;
    facts.wide = wide;
    facts.flow = flowName(instruction.flow);
    facts.target = instruction.target;
    facts.bytes = instruction.bytes;
    FrameNamer frame;
    if (body && !body->frame.empty()) {
      frame = [this, body](std::int64_t offset) {
        return frameSlot(*body, offset);
      };
      facts.frameRegister = body->frameRegister;
      if (const auto depth = stackDepth(*body, instruction)) {
        facts.stackRegister = body->stackRegister;
        facts.stackDepth = *depth;
      }
      facts.frame = &frame;
    }
    for (const auto &[to, kind] : instruction.refs)
      facts.references.push_back(to);
    if (!classic.operands.empty()) {
      text.padTo(base + MnemonicWidth);
      text.append(formatOperands(
          classic.operands, facts,
          [this](std::uint64_t address, NameUse use, std::string_view size) {
            return nameOf(address, use, size);
          }));
    }
    return text;
  }

  void instructionLines(std::vector<Line> &out, const Item &item,
                        std::size_t base) {
    const Function &function = functions[item.function];
    const DecodedFunction &body = decode(item.function);
    const Instruction &instruction = body.instructions[item.index];
    if (item.start == function.entry) {
      // Function header: a rule, the attribute block (each ends with a blank
      // line), exports and the proc line with references.
      addLine(out, item, item.start, "blank", {});
      StyledText rule = lead(base - NameColumnWidth);
      rule.append(SubroutineRule, ListingRole::Separator);
      addLine(out, item, item.start, "separator", std::move(rule));
      addLine(out, item, item.start, "blank", {});
      std::string attributes;
      for (const auto &[present, spelling] :
           {std::pair{function.library, LibraryAttribute},
            std::pair{function.thunk, ThunkAttribute},
            std::pair{!body.frameRegister.empty(), FrameAttribute}})
        if (present)
          attributes += (attributes.empty() ? "" : " ") + std::string(spelling);
      if (!attributes.empty()) {
        StyledText line = lead(base - NameColumnWidth);
        line.append(AttributesLead + attributes, ListingRole::AutoComment);
        addLine(out, item, item.start, "comment", std::move(line));
      }
      addLine(out, item, item.start, "blank", {});
      // The entry point is public like every export.
      if (function.exported || function.entry == imageEntry ||
          runtimeEntries.contains(function.entry)) {
        StyledText publicLine = lead(base);
        publicLine.append("public ", ListingRole::Directive);
        publicLine.append(function.name, ListingRole::CodeName, function.entry);
        addLine(out, item, item.start, "directive", std::move(publicLine));
      }
      StyledText head = lead(base - NameColumnWidth);
      head.append(function.name,
                  function.library ? ListingRole::LibraryName
                                   : ListingRole::CodeName,
                  function.entry);
      head.padTo(base);
      head.append("proc near", ListingRole::Directive);
      addNamedLine(out, item, base - NameColumnWidth, std::move(head),
                   item.start, "header");
      if (!body.frame.empty()) {
        addLine(out, item, item.start, "blank", {});
        for (const auto &[offset, size] : body.frame) {
          StyledText line = lead(base - NameColumnWidth);
          line.append(frameName(body, offset), ListingRole::Plain);
          line.padTo(base);
          line.append("= ", ListingRole::Punctuation);
          line.append(std::string(x86SizeKeyword(size)) + " ptr",
                      ListingRole::Keyword);
          // Offsets align on their sign: `-30h`, ` 10h`.
          line.append(offset < 0 ? " -" : "  ", ListingRole::Punctuation);
          line.append(x86Number(static_cast<std::uint64_t>(
                          offset < 0 ? -offset : offset)),
                      ListingRole::Number);
          addLine(out, item, item.start, "frame", std::move(line));
        }
        addLine(out, item, item.start, "blank", {});
      }
    } else if (body.labels.contains(item.start)) {
      addLine(out, item, item.start, "blank", {});
      StyledText label = lead(base - NameColumnWidth);
      label.append(nameOf(item.start, NameUse::Transfer, {}).text,
                   ListingRole::Label, item.start);
      label.append(":", ListingRole::Punctuation);
      addNamedLine(out, item, base - NameColumnWidth, std::move(label),
                   item.start, "label");
    }
    if (body.unwind && body.unwind->begin == item.start) {
      StyledText open = lead(base - NameColumnWidth);
      open.append(body.unwind->personality.empty()
                      ? std::string(UnwindOpen)
                      : std::string(UnwindOpen) + " // " +
                            body.unwind->personality,
                  ListingRole::AutoComment);
      addLine(out, item, item.start, "comment", std::move(open));
    }
    StyledText text = instructionText(
        instruction, base, opcodeColumn(instruction, base - NameColumnWidth),
        &body);
    const auto comment =
        takeString(neverd_annotation_get(session, instruction.address));
    appendComment(text, comment, ListingRole::Comment, base - NameColumnWidth);
    std::optional<std::uint64_t> target = instruction.target;
    if (!target && instruction.refs.size() == 1)
      target = instruction.refs.front().first;
    addLine(out, item, item.start, "insn", std::move(text), target);
    out.back().flow = std::string(flowName(instruction.flow));
    const std::uint64_t next = item.start + item.size;
    if (body.unwind && body.unwind->end > item.start &&
        body.unwind->end <= next) {
      StyledText close = lead(base - NameColumnWidth);
      close.append(UnwindClose + upperHex(body.unwind->begin),
                   ListingRole::AutoComment);
      addLine(out, item, item.start, "comment", std::move(close));
    }
    if (next >= body.end) {
      // The footer belongs to the last instruction.  Code that no function
      // owns, padding or data follows a rule; a function brings its own.
      StyledText end = lead(base - NameColumnWidth);
      end.append(function.name, ListingRole::CodeName, function.entry);
      end.padTo(base);
      end.append("endp", ListingRole::Directive);
      addLine(out, item, item.start, "footer", std::move(end));
      addLine(out, item, item.start, "blank", {});
      if (next < regions[item.region].end && !functionAtEntry(next)) {
        StyledText rule = lead(base - NameColumnWidth);
        rule.append(SeparatorRule, ListingRole::Separator);
        addLine(out, item, item.start, "separator", std::move(rule));
      }
    } else if (!fallsThrough(instruction.flow)) {
      StyledText rule = lead(base - NameColumnWidth);
      rule.append(SeparatorRule, ListingRole::Separator);
      addLine(out, item, item.start + item.size, "separator", std::move(rule));
    }
  }

  void dataLines(std::vector<Line> &out, const Item &item, std::size_t base) {
    const std::size_t nameBase = base - NameColumnWidth;
    StyledText head = lead(nameBase);
    std::optional<std::uint64_t> target;
    std::string comment;
    const auto named = [&](ListingRole role) {
      const auto name = nameOf(item.start, NameUse::Data, {});
      const bool referenced = [&] {
        auto [b, e] = referencesTo(item.start);
        return b != e;
      }();
      if (role == ListingRole::Plain && !referenced &&
          !dataNames.contains(item.start))
        return;
      head.append(name.text, name.role, item.start);
    };
    switch (item.kind) {
    case ItemKind::Byte: {
      named(ListingRole::Plain);
      head.padTo(base);
      head.append("db", ListingRole::Directive);
      unsigned char value = 0;
      const bool mapped =
          neverd_read_bytes(session, item.start, &value, 1) == 1;
      head.padTo(base + 4);
      head.append(mapped ? x86Number(value) : std::string("?"),
                  ListingRole::Number);
      if (mapped && value >= 0x20 && value < 0x7f)
        comment = std::string(1, static_cast<char>(value));
      break;
    }
    case ItemKind::String: {
      const auto &string = strings[item.index];
      head.append(string.name, ListingRole::DummyDataName, item.start);
      head.padTo(base);
      head.append("db", ListingRole::Directive);
      head.padTo(base + 3);
      head.append(escapeString(string.value, MaxStringDisplay),
                  ListingRole::String);
      break;
    }
    case ItemKind::Slot: {
      const auto &slot = slots.at(item.start);
      head.append(slot.label, ListingRole::ImportName, item.start);
      head.padTo(base);
      head.append(pointerSize == 8 ? "dq" : "dd", ListingRole::Directive);
      head.padTo(base + 3);
      head.append("offset ", ListingRole::Keyword);
      head.append(slot.name, ListingRole::ImportName);
      if (!slot.module.empty())
        comment = slot.module;
      break;
    }
    case ItemKind::Pointer: {
      named(ListingRole::Plain);
      head.padTo(base);
      head.append(pointerSize == 8 ? "dq" : "dd", ListingRole::Directive);
      head.padTo(base + 3);
      head.append("offset ", ListingRole::Keyword);
      const auto to = pointerAt(item.start)->target;
      if (const auto name = nameOf(to, NameUse::Address, {});
          !name.text.empty())
        head.append(name.text, name.role, name.address);
      else
        head.append(dialect == OperandDialect::X86 ? x86Number(to)
                                                   : hexAddress(to),
                    ListingRole::Number);
      target = to;
      // A pointer to a string quotes it.
      if (const auto *string = stringAt(to); string && string->address == to)
        comment = escapeString(string->value, MaxStringDisplay);
      break;
    }
    case ItemKind::Align:
      head.padTo(base);
      head.append("align ", ListingRole::Directive);
      head.append(dialect == OperandDialect::X86 ? x86Number(item.alignment)
                                                 : hexAddress(item.alignment),
                  ListingRole::Number);
      break;
    case ItemKind::Uninitialized:
      named(ListingRole::Plain);
      head.padTo(base);
      head.append("db", ListingRole::Directive);
      head.padTo(base + 3);
      if (item.size == 1) {
        head.append("?", ListingRole::Number);
      } else {
        head.append(x86Number(item.size), ListingRole::Number);
        head.append(" dup(?)", ListingRole::Directive);
      }
      break;
    case ItemKind::Instruction:
      return;
    }
    const auto user = takeString(neverd_annotation_get(session, item.start));
    auto xrefs = xrefComments(item.start);
    if (!user.empty())
      appendComment(head, user, ListingRole::Comment, nameBase);
    else if (!xrefs.empty()) {
      head.padTo(nameBase + CommentColumn);
      head.append("; ", ListingRole::Xref);
      head.append(xrefs.front());
      xrefs.erase(xrefs.begin());
    } else if (!comment.empty())
      appendComment(head, comment, ListingRole::AutoComment, nameBase);
    addLine(out, item, item.start, "data", std::move(head), target);
    for (auto &more : xrefs) {
      StyledText line = lead(nameBase);
      line.padTo(nameBase + CommentColumn);
      line.append("; ", ListingRole::Xref);
      line.append(more);
      addLine(out, item, item.start, "xref", std::move(line));
    }
  }

  std::vector<Line> itemLines(const Item &item) {
    std::vector<Line> lines;
    const std::size_t opcodeWidth =
        opcodeBytes > 0 ? static_cast<std::size_t>(opcodeBytes) * 3 : 0;
    const std::size_t base = opcodeWidth + NameColumnWidth;
    if (item.start == regions[item.region].start)
      segmentHeader(lines, item, base);
    if (item.kind == ItemKind::Instruction)
      instructionLines(lines, item, base);
    else
      dataLines(lines, item, base);
    if (item.start + item.size >= regions[item.region].end) {
      StyledText ends = lead(base - NameColumnWidth);
      ends.append(segmentDirectiveName(regions[item.region]),
                  ListingRole::SegmentName);
      ends.padTo(base);
      ends.append("ends", ListingRole::Directive);
      addLine(lines, item, item.start, "directive", std::move(ends));
      addLine(lines, item, item.start, "blank", {});
    }
    for (std::size_t i = 0; i < lines.size(); ++i)
      lines[i].sub = static_cast<std::uint32_t>(i);
    return lines;
  }

  Json lineJson(const Line &line) {
    const int region =
        regionIndex(line.address < regions.back().end ? line.address
                                                      : regions.back().end - 1);
    const int owner = region >= 0 ? region : regionIndex(line.item);
    Json json = {{"item", hexAddress(line.item)},
                 {"address", hexAddress(line.address)},
                 {"sub", line.sub},
                 {"cls", static_cast<int>(line.cls)},
                 {"kind", line.kind},
                 {"prefix", prefixOf(line.address, owner >= 0 ? owner : 0)},
                 {"text", line.text.text()},
                 {"spans", spansJson(line.text)}};
    if (line.target)
      json["target"] = hexAddress(*line.target);
    if (!line.flow.empty())
      json["flow"] = line.flow;
    if (line.function >= 0 &&
        static_cast<std::size_t>(line.function) < functions.size()) {
      json["function"] = functions[line.function].name;
      json["function_address"] = hexAddress(functions[line.function].entry);
    }
    return json;
  }

  //===--------------------------------------------------------------------===//
  // Reference index
  //===--------------------------------------------------------------------===//

  void indexStep() {
    if (indexState == IndexState::Idle) {
      if (!codeRefs) {
        indexState = IndexState::Unavailable;
        indexError = "This engine does not publish direct references";
        return;
      }
      indexState = IndexState::Building;
      indexCursor = 0;
      indexDone = 0;
      indexTotal = functions.size();
      references.clear();
    }
    if (indexState != IndexState::Building)
      return;
    const char *raw = codeRefs(session, *indexCursor, IndexStepFunctions);
    if (!raw) {
      indexState = IndexState::Unavailable;
      indexError = takeString(neverd_last_error(session));
      references.clear();
      return;
    }
    const auto page = takeJson(raw);
    if (const auto rows = page.value("refs", Json()); rows.is_array())
      for (const auto &row : rows) {
        if (!row.is_array() || row.size() != 3)
          continue;
        const auto kind = parseRefKind(row[2].get<std::string>());
        if (!kind)
          continue;
        references.push_back({jsonAddress(row[1]), jsonAddress(row[0]), *kind});
      }
    const auto next = page.value("next_entry", Json());
    const auto previous = *indexCursor;
    if (next.is_string()) {
      indexCursor = jsonAddress(next);
      const auto from =
          std::lower_bound(functions.begin(), functions.end(), previous,
                           [](const Function &f, std::uint64_t value) {
                             return f.entry < value;
                           });
      const auto to =
          std::lower_bound(functions.begin(), functions.end(), *indexCursor,
                           [](const Function &f, std::uint64_t value) {
                             return f.entry < value;
                           });
      indexDone +=
          static_cast<std::size_t>(std::max<std::ptrdiff_t>(1, to - from));
      return;
    }
    // Each relocated data slot refers to its pointer.
    if (pointerRefs)
      for (std::optional<std::uint64_t> cursor = 0; cursor;) {
        const auto page = takeJson(pointerRefs(session, *cursor, PointerPage));
        cursor.reset();
        if (const auto rows = page.value("refs", Json()); rows.is_array())
          for (const auto &row : rows)
            if (row.is_array() && row.size() == 3)
              references.push_back(
                  {jsonAddress(row[1]), jsonAddress(row[0]), RefKind::Offset});
        if (const auto next = page.value("next_slot", Json()); next.is_string())
          cursor = jsonAddress(next);
      }
    std::sort(references.begin(), references.end(),
              [](const Reference &a, const Reference &b) {
                return a.to != b.to ? a.to < b.to : a.from < b.from;
              });
    references.erase(std::unique(references.begin(), references.end(),
                                 [](const Reference &a, const Reference &b) {
                                   return a.to == b.to && a.from == b.from &&
                                          a.kind == b.kind;
                                 }),
                     references.end());
    indexDone = indexTotal;
    indexState = IndexState::Ready;
    // Labels and reference comments are now available.
    decoded.clear();
    decodedOrder.clear();
    ++generation;
  }

  const char *indexStateName() const {
    switch (indexState) {
    case IndexState::Idle:
      return "pending";
    case IndexState::Building:
      return "building";
    case IndexState::Ready:
      return "ready";
    case IndexState::Unavailable:
      return "unavailable";
    }
    return "unavailable";
  }
};

Listing::Listing(neverd_session_t session)
    : impl_(std::make_unique<Impl>(session)) {}
Listing::~Listing() = default;

void Listing::invalidate() {
  impl_->built = false;
  ++impl_->generation;
}

std::uint64_t Listing::generation() const { return impl_->generation; }

bool Listing::hasIdleWork() const {
  return (impl_->discoverFunctions && !impl_->discovered) ||
         impl_->indexState == Impl::IndexState::Idle ||
         impl_->indexState == Impl::IndexState::Building;
}

void Listing::idleStep() {
  // Function discovery comes first, so the reference index covers its
  // functions.
  if (!impl_->discovered) {
    impl_->discover();
    impl_->build();
    return;
  }
  impl_->build();
  impl_->indexStep();
}

Json Listing::indexState() const {
  return {{"state", impl_->indexStateName()},
          {"done", impl_->indexDone},
          {"total", impl_->indexTotal},
          {"references", impl_->indexState == Impl::IndexState::Ready
                             ? impl_->references.size()
                             : 0},
          {"generation", std::to_string(impl_->generation)},
          {"functions", impl_->functions.size()}};
}

Json Listing::page(const Json &payload) {
  auto &d = *impl_;
  d.build();
  if (d.regions.empty())
    return {{"lines", Json::array()},
            {"anchor", 0},
            {"at_start", true},
            {"at_end", true},
            {"generation", std::to_string(d.generation)}};
  d.opcodeBytes =
      static_cast<int>(sizeField(payload, "opcode_bytes", 0, MaxOpcodeBytes));
  const auto before = sizeField(payload, "before", 0, MaxPageLines);
  const auto after = sizeField(payload, "after", 100, MaxPageLines);
  std::uint64_t address =
      payload.contains("address")
          ? parseAddress(stringField(payload, "address", {}, 18))
          : d.regions.front().start;
  std::size_t sub = sizeField(payload, "sub", 0, 1 << 20);
  // An unmapped address snaps to the following region (or the last one).
  if (d.regionIndex(address) < 0) {
    auto it = std::upper_bound(
        d.regions.begin(), d.regions.end(), address,
        [](std::uint64_t value, const Region &r) { return value < r.start; });
    address = it != d.regions.end() ? it->start : d.regions.back().end - 1;
    sub = 0;
  }
  auto anchor = d.itemAt(address);
  if (!anchor)
    throw Error("invalid_address", "Address is outside the mapped image");
  std::vector<Line> forward = d.itemLines(*anchor);
  // A sub-line past the item's last line continues with the next item, so a
  // client can page forward from its last loaded line.
  while (sub >= forward.size()) {
    auto following = d.nextItem(*anchor);
    if (!following) {
      sub = forward.empty() ? 0 : forward.size() - 1;
      break;
    }
    anchor = following;
    forward = d.itemLines(*anchor);
    sub = 0;
  }
  std::vector<Line> backward(forward.begin(), forward.begin() + sub);
  forward.erase(forward.begin(), forward.begin() + sub);
  std::optional<Item> next = anchor, previous = anchor;
  bool atEnd = false, atStart = false;
  while (forward.size() < after) {
    next = d.nextItem(*next);
    if (!next) {
      atEnd = true;
      break;
    }
    auto lines = d.itemLines(*next);
    for (auto &line : lines)
      forward.push_back(std::move(line));
  }
  if (!atEnd && !d.nextItem(next ? *next : *anchor))
    atEnd = forward.size() <= after;
  // Backward lines are collected newest-first, then reversed.
  std::vector<Line> reversed(backward.rbegin(), backward.rend());
  while (reversed.size() < before) {
    previous = d.previousItem(*previous);
    if (!previous) {
      atStart = true;
      break;
    }
    auto lines = d.itemLines(*previous);
    for (auto it = lines.rbegin(); it != lines.rend(); ++it)
      reversed.push_back(std::move(*it));
  }
  if (!atStart && reversed.empty() && sub == 0 && !d.previousItem(*anchor))
    atStart = true;
  if (reversed.size() > before)
    reversed.resize(before);
  if (forward.size() > after)
    forward.resize(after);
  Json lines = Json::array();
  for (auto it = reversed.rbegin(); it != reversed.rend(); ++it)
    lines.push_back(d.lineJson(*it));
  const auto anchorIndex = lines.size();
  for (const auto &line : forward)
    lines.push_back(d.lineJson(line));
  return {{"lines", std::move(lines)},
          {"anchor", anchorIndex},
          {"at_start", atStart},
          {"at_end", atEnd},
          {"generation", std::to_string(d.generation)},
          {"index", indexState()}};
}

Json Listing::overview(const Json &payload) {
  auto &d = *impl_;
  d.build();
  const auto buckets = std::max<std::size_t>(
      1, sizeField(payload, "buckets", 1024, MaxOverviewBuckets));
  std::uint64_t total = 0;
  Json regions = Json::array();
  for (const auto &region : d.regions) {
    total += region.end - region.start;
    Json entry = {{"name", region.name},
                  {"start", hexAddress(region.start)},
                  {"end", hexAddress(region.end)},
                  {"initialized_end", hexAddress(region.initializedEnd)},
                  {"linear", hexAddress(region.linear)},
                  {"exec", region.exec}};
    if (region.fileOffset)
      entry["file_offset"] = hexAddress(*region.fileOffset);
    regions.push_back(std::move(entry));
  }
  std::string classes(buckets, '0');
  if (total) {
    const auto classify = [&](std::uint64_t linear) -> AddressClass {
      auto it = std::upper_bound(d.regions.begin(), d.regions.end(), linear,
                                 [](std::uint64_t value, const Region &r) {
                                   return value < r.linear;
                                 });
      if (it == d.regions.begin())
        return AddressClass::Unmapped;
      --it;
      const std::uint64_t address = it->start + (linear - it->linear);
      if (address >= it->end)
        return AddressClass::Unmapped;
      if (address >= it->initializedEnd)
        return AddressClass::Data;
      if (it->exec) {
        const int f = d.functionIndexCoarse(address);
        if (f < 0)
          return AddressClass::Unexplored;
        return d.functions[f].thunk     ? AddressClass::External
               : d.functions[f].library ? AddressClass::LibraryFunction
                                        : AddressClass::RegularFunction;
      }
      for (std::uint64_t back = 0; back < d.pointerSize && back <= address;
           ++back)
        if (d.slots.contains(address - back))
          return AddressClass::External;
      if (d.stringAt(address) || d.dataNames.contains(address))
        return AddressClass::Data;
      return AddressClass::Unexplored;
    };
    const auto rank = [](AddressClass cls) {
      switch (cls) {
      case AddressClass::LibraryFunction:
        return 6;
      case AddressClass::RegularFunction:
        return 5;
      case AddressClass::External:
        return 4;
      case AddressClass::Instruction:
        return 3;
      case AddressClass::Data:
        return 2;
      case AddressClass::Unexplored:
        return 1;
      case AddressClass::Unmapped:
        return 0;
      }
      return 0;
    };
    for (std::size_t b = 0; b < buckets; ++b) {
      AddressClass best = AddressClass::Unmapped;
      for (int s = 0; s < OverviewSamples; ++s) {
        const long double position =
            (static_cast<long double>(b) + (s + 0.5L) / OverviewSamples) /
            static_cast<long double>(buckets);
        const auto linear = std::min<std::uint64_t>(
            total - 1, static_cast<std::uint64_t>(position * total));
        const auto cls = classify(linear);
        if (rank(cls) > rank(best))
          best = cls;
      }
      classes[b] = static_cast<char>('0' + static_cast<int>(best));
    }
  }
  return {{"buckets", classes},
          {"total", hexAddress(total)},
          {"regions", std::move(regions)},
          {"generation", std::to_string(d.generation)}};
}

Json Listing::references(std::uint64_t address, const Json &payload) {
  auto &d = *impl_;
  d.discover();
  d.build();
  const auto direction = stringField(payload, "direction", "to", 8);
  const auto offset =
      sizeField(payload, "offset", 0, std::numeric_limits<std::size_t>::max());
  const auto limit = std::max<std::size_t>(
      1, sizeField(payload, "limit", 128, MaxReferencePage));
  Json items = Json::array();
  std::size_t total = 0;
  const auto row = [&](std::uint64_t from, std::uint64_t to, RefKind kind) {
    const int f = d.functionIndexCoarse(direction == "to" ? from : to);
    Json item = {{"address", hexAddress(direction == "to" ? from : to)},
                 {"from", hexAddress(from)},
                 {"to", hexAddress(to)},
                 {"kind", refKindName(kind)},
                 {"type", std::string(1, refKindLetter(kind))},
                 {"text", d.locationText(direction == "to" ? from : to)},
                 {"function", f >= 0 ? d.functions[f].name : std::string()}};
    if (f >= 0)
      item["function_address"] = hexAddress(d.functions[f].entry);
    return item;
  };
  if (direction == "to") {
    // An explicit request finishes the background index now; it is a
    // parallel decode of every function and completes quickly.
    while (d.indexState == Impl::IndexState::Idle ||
           d.indexState == Impl::IndexState::Building)
      d.indexStep();
    if (d.indexState == Impl::IndexState::Unavailable)
      throw Error("unsupported", d.indexError.empty()
                                     ? "References are unavailable"
                                     : d.indexError);
    auto [begin, end] = d.referencesTo(address);
    total = static_cast<std::size_t>(end - begin);
    for (auto it = begin + std::min(offset, total);
         it != end && items.size() < limit; ++it)
      items.push_back(row(it->from, it->to, it->kind));
  } else if (direction == "from") {
    std::vector<std::pair<std::uint64_t, RefKind>> refs;
    if (auto item = d.itemAt(address);
        item && item->kind == ItemKind::Instruction) {
      const auto &instruction =
          d.decode(item->function).instructions[item->index];
      if (instruction.target) {
        const auto kind = instruction.flow == Flow::Call   ? RefKind::Call
                          : instruction.flow == Flow::Jump ? RefKind::Jump
                                                           : RefKind::CondJump;
        refs.push_back({*instruction.target, kind});
      }
      for (const auto &ref : instruction.refs)
        refs.push_back(ref);
      address = instruction.address;
    }
    total = refs.size();
    for (std::size_t i = std::min(offset, total);
         i < total && items.size() < limit; ++i)
      items.push_back(row(address, refs[i].first, refs[i].second));
  } else {
    throw Error("invalid_request", "direction must be to or from");
  }
  const bool complete = offset >= total || items.size() >= total - offset;
  return {{"items", std::move(items)},
          {"total", total},
          {"offset", offset},
          {"next_offset", complete ? Json(nullptr) : Json(offset + limit)},
          {"complete", complete},
          {"source", "direct_references"}};
}

std::optional<std::uint64_t> Listing::resolveName(const std::string &name) {
  auto &d = *impl_;
  d.build();
  if (auto it = d.listingNames.find(name); it != d.listingNames.end())
    return it->second;
  if (auto address = parseDummyName(name);
      address && d.regionIndex(*address) >= 0)
    return address;
  return std::nullopt;
}

Json Listing::blockLines(std::uint64_t start, std::uint64_t end) {
  auto &d = *impl_;
  d.build();
  Json lines = Json::array();
  const int f = d.functionIndex(start);
  if (f < 0)
    return lines;
  const auto &body = d.decode(f);
  auto it =
      std::lower_bound(body.instructions.begin(), body.instructions.end(),
                       start, [](const Instruction &i, std::uint64_t value) {
                         return i.address < value;
                       });
  const auto emit = [&](std::uint64_t address, const StyledText &text,
                        const char *kind) {
    lines.push_back({{"address", hexAddress(address)},
                     {"kind", kind},
                     {"text", text.text()},
                     {"spans", spansJson(text)}});
  };
  const auto &function = d.functions[f];
  if (start == function.entry) {
    StyledText head;
    head.append(function.name,
                function.library ? ListingRole::LibraryName
                                 : ListingRole::CodeName,
                function.entry);
    head.append(" proc near", ListingRole::Directive);
    emit(start, head, "header");
  } else if (body.labels.contains(start)) {
    StyledText label;
    label.append(d.nameOf(start, NameUse::Transfer, {}).text,
                 ListingRole::Label, start);
    label.append(":", ListingRole::Punctuation);
    emit(start, label, "label");
  }
  for (; it != body.instructions.end() && it->address < end; ++it) {
    StyledText text = d.instructionText(*it, 0, {}, &body);
    const auto comment =
        takeString(neverd_annotation_get(d.session, it->address));
    if (!comment.empty())
      d.appendComment(text, comment, ListingRole::Comment, 0);
    emit(it->address, text, "insn");
  }
  return lines;
}

const Json &Listing::functionRows() {
  auto &d = *impl_;
  d.build();
  if (d.functionRowsGeneration != d.generation ||
      d.functionRowsCache.is_null()) {
    Json rows = Json::array();
    for (const auto &function : d.functions) {
      Json row = {{"name", function.name},
                  {"address", hexAddress(function.entry)},
                  {"size", function.end - function.entry},
                  {"library", function.library},
                  {"thunk", function.thunk},
                  {"exported", function.exported}};
      if (function.engineName != function.name)
        row["engine_name"] = function.engineName;
      rows.push_back(std::move(row));
    }
    d.functionRowsCache = std::move(rows);
    d.functionRowsGeneration = d.generation;
  }
  return d.functionRowsCache;
}

const std::unordered_map<std::string, std::string> &Listing::functionAliases() {
  impl_->build();
  return impl_->aliases;
}

Json Listing::names() {
  auto &d = *impl_;
  d.build();
  Json items = Json::array();
  for (const auto &function : d.functions)
    items.push_back({{"name", function.name},
                     {"address", hexAddress(function.entry)},
                     {"kind", function.thunk     ? "thunk"
                              : function.library ? "library"
                                                 : "function"}});
  for (const auto &[address, name] : d.dataNames)
    items.push_back(
        {{"name", name}, {"address", hexAddress(address)}, {"kind", "data"}});
  for (const auto &[address, slot] : d.slots)
    items.push_back({{"name", slot.label},
                     {"address", hexAddress(address)},
                     {"kind", "import"}});
  for (const auto &string : d.strings)
    items.push_back({{"name", string.name},
                     {"address", hexAddress(string.address)},
                     {"kind", "string"}});
  return items;
}

Json Listing::regions() {
  auto &d = *impl_;
  d.build();
  Json items = Json::array();
  for (const auto &region : d.regions) {
    std::string flags;
    flags += region.read ? 'R' : '-';
    flags += region.write ? 'W' : '-';
    flags += region.exec ? 'X' : '-';
    items.push_back({{"name", region.name},
                     {"address", hexAddress(region.start)},
                     {"start", hexAddress(region.start)},
                     {"end", hexAddress(region.end)},
                     {"size", hexAddress(region.end - region.start)},
                     {"initialized_end", hexAddress(region.initializedEnd)},
                     {"flags", flags},
                     {"alignment", region.alignment},
                     {"class", region.exec                             ? "CODE"
                               : region.initializedEnd == region.start ? "BSS"
                               : region.write ? "DATA"
                                              : "CONST"}});
  }
  return items;
}

} // namespace neverd::worker
