//===- NeverDCAPIPersist.cpp - C API: annotations and renames -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-address annotations and persistent function renaming.
///
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "neverd/support/AtomicOutput.h"
#include "neverd/support/ProjectWriteLock.h"
#include "neverd/support/StringScan.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <vector>

using namespace neverd;
using namespace neverd::sdk;

// ===--------------------------------------------------------------------===//
// Annotations
// ===--------------------------------------------------------------------===//

static std::string annotationPath(const Session *S) {
  return S->FilePath.string() + ".neverd-annotations.json";
}

static bool saveSidecar(Session *S, llvm::StringRef Path,
                        llvm::json::Array Values, llvm::StringRef Kind) {
  auto Temporary = llvm::sys::fs::TempFile::create(
      (Path + ".tmp-%%%%%%").str(),
      llvm::sys::fs::owner_read | llvm::sys::fs::owner_write);
  if (!Temporary) {
    S->setError("cannot create " + Kind.str() +
                " temporary file: " + llvm::toString(Temporary.takeError()));
    return false;
  }
  std::error_code WriteError;
  {
    // TempFile owns the descriptor. Clear a handled stream error before its
    // destructor so ENOSPC/EFBIG becomes a C ABI error rather than LLVM abort.
    llvm::raw_fd_ostream OS(Temporary->FD, false);
    OS << llvm::json::Value(std::move(Values));
    OS.flush();
    WriteError = OS.error();
    OS.clear_error();
  }
  if (WriteError) {
    llvm::consumeError(
        support::atomic_output::discardTemporaryOutput(*Temporary));
    S->setError("cannot write " + Kind.str() + ": " + WriteError.message());
    return false;
  }
  if (auto Error = support::atomic_output::closeAndCommitTemporaryOutput(
          *Temporary, Path)) {
    S->setError("cannot save " + Kind.str() + ": " +
                llvm::toString(std::move(Error)));
    return false;
  }
  return true;
}

static std::optional<va_t>
parsePersistedAddress(const llvm::json::Value &Value) {
  if (auto Str = Value.getAsString()) {
    llvm::StringRef Ref(*Str);
    if (Ref.empty() || Ref.front() == '-')
      return std::nullopt;
    if (Ref.consume_front("0x") || Ref.consume_front("0X")) {
      if (Ref.empty())
        return std::nullopt;
    }
    va_t Addr = 0;
    if (Ref.getAsInteger(16, Addr))
      return std::nullopt;
    return Addr;
  }

  if (auto Integer = Value.getAsUINT64())
    return *Integer;

  if (auto Num = Value.getAsNumber()) {
    if (!std::isfinite(*Num) || *Num < 0.0 || *Num >= 18446744073709551616.0 ||
        std::trunc(*Num) != *Num)
      return std::nullopt;
    return static_cast<va_t>(*Num);
  }
  return std::nullopt;
}

void neverd_annotation_set(neverd_session_t Sess, neverd_va_t Addr,
                           const char *Text) {
  auto *S = toSession(Sess);
  if (Text && Text[0])
    S->Annotations[Addr] = Text;
  else
    S->Annotations.erase(Addr);
}

void neverd_annotation_remove(neverd_session_t Sess, neverd_va_t Addr) {
  toSession(Sess)->Annotations.erase(Addr);
}

const char *neverd_annotation_get(neverd_session_t Sess, neverd_va_t Addr) {
  auto *S = toSession(Sess);
  auto It = S->Annotations.find(Addr);
  if (It == S->Annotations.end())
    return nullptr;
  return dupStr(It->second);
}

const char *neverd_annotations_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  llvm::json::Array Arr;
  for (const auto &[Addr, Text] : S->Annotations) {
    llvm::json::Object Obj;
    Obj["addr"] = vaHex(Addr);
    Obj["text"] = Text;
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

int neverd_annotations_save(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return 1;
  }
  ProjectWriteLock Lock(S->FilePath);
  if (!Lock) {
    S->setError("project writer unavailable: " + Lock.error());
    return 1;
  }
  llvm::json::Array Arr;
  for (const auto &[Addr, Text] : S->Annotations) {
    llvm::json::Object Obj;
    // Store the address as a hex string, mirroring neverd_annotations_json,
    // to preserve full address precision across JSON consumers.
    Obj["addr"] = vaHex(Addr);
    Obj["text"] = Text;
    Arr.push_back(std::move(Obj));
  }
  return saveSidecar(S, annotationPath(S), std::move(Arr), "annotations") ? 0
                                                                          : 1;
}

int neverd_annotations_load(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  S->clearError();
  if (!S->Loaded)
    return 1;
  auto Path = annotationPath(S);
  std::error_code EC;
  const bool Exists = std::filesystem::exists(Path, EC);
  if (EC) {
    S->setError("cannot inspect annotations: " + EC.message());
    return 1;
  }
  if (!Exists) {
    S->Annotations.clear();
    return 0;
  }
  std::ifstream In(Path);
  if (!In.is_open()) {
    S->setError("cannot read annotations");
    return 1;
  }
  std::string Content((std::istreambuf_iterator<char>(In)),
                      std::istreambuf_iterator<char>());
  auto Parsed = llvm::json::parse(Content);
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    S->setError("annotations sidecar is not valid JSON");
    return 1;
  }
  auto *Arr = Parsed->getAsArray();
  if (!Arr)
    return 1;
  S->Annotations.clear();
  for (const auto &Item : *Arr) {
    auto *Obj = Item.getAsObject();
    if (!Obj)
      continue;
    const llvm::json::Value *AddrVal = Obj->get("addr");
    auto Text = Obj->getString("text");
    if (!AddrVal || !Text)
      continue;
    std::optional<va_t> Addr = parsePersistedAddress(*AddrVal);
    if (Addr)
      S->Annotations[*Addr] = Text->str();
  }
  return 0;
}

// ===--------------------------------------------------------------------===//
// Symbol renaming
// ===--------------------------------------------------------------------===//

int neverd_rename_func(neverd_session_t Sess, const char *OldName,
                       const char *NewName) {
  auto *S = toSession(Sess);
  if (!S->Loaded || !OldName || !NewName)
    return -1;
  (void)neverd_func_count(Sess);

  for (auto &F : S->Functions) {
    if (F.Name == OldName) {
      const auto PreviousName = F.Name;
      const auto PreviousOrigin = F.Origin;
      const auto PreviousRenames = S->Renames;
      S->Renames[F.Entry] = NewName;
      F.Name = NewName;
      F.Origin = NameOrigin::User;
      if (neverd_renames_save(Sess) != 0) {
        S->Renames = PreviousRenames;
        F.Name = PreviousName;
        F.Origin = PreviousOrigin;
        return -1;
      }
      return 0;
    }
  }
  S->setError("function not found: " + std::string(OldName));
  return -1;
}

int neverd_rename_addr(neverd_session_t Sess, neverd_va_t Addr,
                       const char *Name) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return -1;
  }
  const llvm::StringRef NewName = Name ? Name : "";
  constexpr size_t MaxNameBytes = 4096;
  if (NewName.size() > MaxNameBytes ||
      llvm::any_of(NewName,
                   [](unsigned char C) { return C <= ' ' || C == 0x7f; })) {
    S->setError("a name has at most 4096 bytes and no spaces or control "
                "characters");
    return -1;
  }
  if (!S->Img.readVA(Addr, 1)) {
    S->setError(vaHex(Addr) + " is not in the image");
    return -1;
  }
  (void)neverd_func_count(Sess);
  const auto PreviousRenames = S->Renames;
  const auto PreviousOriginals = S->OriginalNames;
  if (NewName.empty()) {
    S->Renames.erase(Addr);
  } else {
    // What the address was called before the user named it.
    if (!S->OriginalNames.count(Addr))
      if (const Symbol *Sym = S->Img.findSymbolAt(Addr); Sym && !Sym->IsFunc)
        S->OriginalNames[Addr] = Sym->Name;
    S->Renames[Addr] = NewName.str();
  }
  // A function entry takes the name in the function list too.
  S->refreshFunctionNames();
  if (neverd_renames_save(Sess) != 0) {
    S->Renames = PreviousRenames;
    S->OriginalNames = PreviousOriginals;
    S->refreshFunctionNames();
    return -1;
  }
  return 0;
}

const char *neverd_renames_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  llvm::json::Array Arr;
  for (const auto &[Addr, NewName] : S->Renames) {
    llvm::json::Object Obj;
    Obj["addr"] = vaHex(Addr);
    auto It = S->OriginalNames.find(Addr);
    Obj["original"] = It != S->OriginalNames.end() ? It->second : "";
    Obj["renamed"] = NewName;
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

int neverd_renames_save(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  S->clearError();
  if (!S->Loaded)
    return -1;
  ProjectWriteLock Lock(S->FilePath);
  if (!Lock) {
    S->setError("project writer unavailable: " + Lock.error());
    return -1;
  }
  auto Path = S->FilePath.string() + ".neverd-renames.json";
  llvm::json::Array Arr;
  for (const auto &[Addr, NewName] : S->Renames) {
    llvm::json::Object Obj;
    Obj["addr"] = vaHex(Addr);
    auto It = S->OriginalNames.find(Addr);
    Obj["original"] = It != S->OriginalNames.end() ? It->second : "";
    Obj["renamed"] = NewName;
    Arr.push_back(std::move(Obj));
  }
  return saveSidecar(S, Path, std::move(Arr), "renames") ? 0 : -1;
}

int neverd_renames_load(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  S->clearError();
  if (!S->Loaded)
    return -1;
  auto Path = S->FilePath.string() + ".neverd-renames.json";
  auto Reset = [&] {
    for (auto &F : S->Functions) {
      if (S->Renames.find(F.Entry) == S->Renames.end())
        continue;
      if (auto Original = S->OriginalNames.find(F.Entry);
          Original != S->OriginalNames.end())
        F.Name = Original->second;
    }
    S->Renames.clear();
    S->refreshFunctionNames();
  };
  std::error_code EC;
  const bool Exists = std::filesystem::exists(Path, EC);
  if (EC) {
    S->setError("cannot inspect renames: " + EC.message());
    return -1;
  }
  if (!Exists) {
    Reset();
    return 0;
  }
  std::ifstream In(Path);
  if (!In.is_open()) {
    S->setError("cannot read renames");
    return -1;
  }
  std::string Content((std::istreambuf_iterator<char>(In)),
                      std::istreambuf_iterator<char>());
  auto Parsed = llvm::json::parse(Content);
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    S->setError("renames sidecar is not valid JSON");
    return -1;
  }
  auto *Arr = Parsed->getAsArray();
  if (!Arr)
    return -1;
  Reset();
  for (const auto &V : *Arr) {
    auto *Obj = V.getAsObject();
    if (!Obj)
      continue;
    const llvm::json::Value *AddrVal = Obj->get("addr");
    auto Renamed = Obj->getString("renamed");
    if (!AddrVal || !Renamed)
      continue;
    std::optional<va_t> Addr = parsePersistedAddress(*AddrVal);
    if (!Addr)
      continue;
    S->Renames[*Addr] = Renamed->str();
    for (auto &F : S->Functions) {
      if (F.Entry == *Addr) {
        F.Name = Renamed->str();
        F.Origin = NameOrigin::User;
        break;
      }
    }
  }
  return 0;
}

// ===--------------------------------------------------------------------===//
// Function edits
// ===--------------------------------------------------------------------===//

static std::string functionsPath(const Session *S) {
  return S->FilePath.string() + ".neverd-functions.json";
}

static llvm::json::Array functionEditRows(const Session *S) {
  llvm::json::Array Rows;
  for (const auto &[Entry, Created] : S->FunctionEdits)
    Rows.push_back(llvm::json::Object{
        {"addr", vaHex(Entry)}, {"state", Created ? "created" : "deleted"}});
  return Rows;
}

int neverd_func_create(neverd_session_t Sess, neverd_va_t Entry) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return -1;
  }
  const Segment *Code = S->Img.getSegmentFor(Entry);
  if (!Code || !Code->isExecutable()) {
    S->setError(vaHex(Entry) + " is not in executable code");
    return -1;
  }
  // The user's last edit at an address decides over the image, the detector
  // and analysis, so a deleted function made again is a created one.
  if (!S->isDeletedFunction(Entry)) {
    (void)S->synchronizeFunctions();
    for (const FuncInfo &F : S->Functions)
      if (F.Entry == Entry) {
        S->setError("a function already starts at " + vaHex(Entry));
        return -1;
      }
  }
  S->FunctionEdits[Entry] = true;
  S->invalidatePipeline();
  return 0;
}

int neverd_func_delete(neverd_session_t Sess, neverd_va_t Entry) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return -1;
  }
  (void)S->synchronizeFunctions();
  if (llvm::none_of(S->Functions,
                    [&](const FuncInfo &F) { return F.Entry == Entry; })) {
    S->setError("no function starts at " + vaHex(Entry));
    return -1;
  }
  // Recorded for a created function too: the image, the detector or analysis
  // may find a function there as well.
  S->FunctionEdits[Entry] = false;
  S->invalidatePipeline();
  return 0;
}

const char *neverd_functions_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  return dupStr(jsonToString(llvm::json::Value(functionEditRows(S))));
}

int neverd_functions_save(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->Loaded)
    return -1;
  ProjectWriteLock Lock(S->FilePath);
  if (!Lock) {
    S->setError("project writer unavailable: " + Lock.error());
    return -1;
  }
  return saveSidecar(S, functionsPath(S), functionEditRows(S), "function edits")
             ? 0
             : -1;
}

int neverd_functions_load(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->Loaded)
    return -1;
  const auto Path = functionsPath(S);
  std::error_code EC;
  const bool Exists = std::filesystem::exists(Path, EC);
  if (EC) {
    S->setError("cannot inspect function edits: " + EC.message());
    return -1;
  }
  std::map<va_t, bool> Edits;
  if (Exists) {
    std::ifstream In(Path);
    if (!In.is_open()) {
      S->setError("cannot read function edits");
      return -1;
    }
    const std::string Content((std::istreambuf_iterator<char>(In)),
                              std::istreambuf_iterator<char>());
    auto Parsed = llvm::json::parse(Content);
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
      S->setError("function edits sidecar is not valid JSON");
      return -1;
    }
    const auto *Rows = Parsed->getAsArray();
    if (!Rows) {
      S->setError("function edits sidecar is not an array");
      return -1;
    }
    for (const auto &Row : *Rows) {
      const auto *Object = Row.getAsObject();
      const llvm::json::Value *Address = Object ? Object->get("addr") : nullptr;
      const auto State = Object ? Object->getString("state") : std::nullopt;
      const auto Entry =
          Address ? parsePersistedAddress(*Address) : std::optional<va_t>();
      if (!Entry || !State || (*State != "created" && *State != "deleted")) {
        S->setError("function edits sidecar has an invalid row");
        return -1;
      }
      Edits[*Entry] = *State == "created";
    }
  }
  if (Edits == S->FunctionEdits)
    return 0;
  S->FunctionEdits = std::move(Edits);
  S->invalidatePipeline();
  return 0;
}

// ===--------------------------------------------------------------------===//
// Data items
// ===--------------------------------------------------------------------===//

namespace {
#define NEVERD_DATA_ITEM_KIND(Id, Spelling)                                    \
  constexpr llvm::StringLiteral Id(Spelling);
#include "neverd/DataNames.def"

/// The bytes of a value of the size \p Kind names (`qword`), or 0.
uint64_t sizedItemBytes(llvm::StringRef Kind) {
#define NEVERD_DATA_SIZE_NAME(SizeKeyword, Bytes, Prefix)                      \
  if (Kind == SizeKeyword)                                                     \
    return Bytes;
#include "neverd/DataNames.def"
  return 0;
}

std::string itemsPath(const Session *S) {
  return S->FilePath.string() + ".neverd-items.json";
}

#define NEVERD_OPERAND_BASE(Id, Spelling)                                      \
  constexpr llvm::StringLiteral k##Id##Base(Spelling);
#include "neverd/OperandFormats.def"

/// The operands of one instruction a format may name.
constexpr int MaxFormattedOperands = 8;

std::string operandsPath(const Session *S) {
  return S->FilePath.string() + ".neverd-operands.json";
}

bool isOperandBase(llvm::StringRef Base) {
#define NEVERD_OPERAND_BASE(Id, Spelling)                                      \
  if (Base == Spelling)                                                        \
    return true;
#include "neverd/OperandFormats.def"
  return false;
}

/// The format \p Object describes, or none with \p Error.
std::optional<Session::OperandFormat>
parseOperandFormat(const llvm::json::Object &Object, std::string &Error) {
  Session::OperandFormat Format;
  const auto Base = Object.getString("base");
  if (!Base || !isOperandBase(*Base)) {
    Error = "an operand's base is number, hex, decimal, binary, char or "
            "offset";
    return std::nullopt;
  }
  Format.Base = Base->str();
  for (const auto &[Key, Field] : {std::pair{"negate", &Format.Negate},
                                   std::pair{"invert", &Format.Invert}})
    if (const llvm::json::Value *Value = Object.get(Key)) {
      const auto Flag = Value->getAsBoolean();
      if (!Flag) {
        Error = std::string(Key) + " is true or false";
        return std::nullopt;
      }
      *Field = *Flag;
    }
  return Format;
}

llvm::json::Array operandFormatRows(const Session *S) {
  llvm::json::Array Rows;
  for (const auto &[Addr, Operands] : S->OperandFormats) {
    llvm::json::Array List;
    for (const auto &[Index, Format] : Operands)
      List.push_back(
          llvm::json::Object{{"operand", static_cast<int64_t>(Index)},
                             {"base", Format.Base},
                             {"negate", Format.Negate},
                             {"invert", Format.Invert}});
    Rows.push_back(llvm::json::Object{{"addr", vaHex(Addr)},
                                      {"operands", std::move(List)}});
  }
  return Rows;
}

/// The item \p Row describes at \p Addr, checked against the image; none
/// with \p Error.
std::optional<Session::DataItem> parseDataItem(const Session &S, va_t Addr,
                                               const llvm::json::Object &Row,
                                               std::string &Error) {
  Session::DataItem Item;
  const auto Kind = Row.getString("kind");
  if (!Kind) {
    Error = "a data item needs a kind";
    return std::nullopt;
  }
  Item.Kind = Kind->str();
  const auto Size = Row.getInteger("size");
  std::optional<strings::Encoding> Encoding;
  if (const uint64_t Bytes = sizedItemBytes(*Kind)) {
    if (Size && static_cast<uint64_t>(*Size) != Bytes) {
      Error = Item.Kind + " takes " + std::to_string(Bytes) + " bytes";
      return std::nullopt;
    }
    Item.Size = Bytes;
  } else if (*Kind == StringItemKind) {
    const auto Name = Row.getString("encoding");
    Encoding = Name ? strings::encodingNamed(*Name) : std::nullopt;
    if (!Encoding) {
      Error = "a string item needs a known encoding";
      return std::nullopt;
    }
    Item.Encoding = strings::encodingName(*Encoding).str();
    const unsigned Unit = strings::encodingUnitBytes(*Encoding);
    if (!Size || *Size < Unit || *Size % Unit) {
      Error = "a string item needs its size in whole units, its terminator "
              "included";
      return std::nullopt;
    }
    Item.Size = static_cast<uint64_t>(*Size);
  } else if (*Kind == UndefinedItemKind) {
    if (!Size || *Size < 1) {
      Error = "an undefined item needs its size";
      return std::nullopt;
    }
    Item.Size = static_cast<uint64_t>(*Size);
  } else {
    Error = "unknown data item kind '" + Item.Kind + "'";
    return std::nullopt;
  }
  const Segment *Seg = S.Img.getSegmentFor(Addr);
  if (!Seg || Addr < Seg->VA || Item.Size > Seg->VA + Seg->Size - Addr) {
    Error = vaHex(Addr) + " to " + vaHex(Addr + Item.Size) +
            " is not in one segment";
    return std::nullopt;
  }
  if (Encoding) {
    // The string's last code unit is its terminator.
    const unsigned Unit = strings::encodingUnitBytes(*Encoding);
    const uint8_t *Last = S.Img.readVA(Addr + Item.Size - Unit, Unit);
    if (!Last || !std::all_of(Last, Last + Unit,
                              [](uint8_t Byte) { return Byte == 0; })) {
      Error = "the string at " + vaHex(Addr) + " does not end in a zero unit";
      return std::nullopt;
    }
  }
  return Item;
}

/// The address of an item in \p Items other than \p Except that shares a
/// byte with [\p Addr, \p Addr + \p Size).
std::optional<va_t>
overlappingItem(const std::map<va_t, Session::DataItem> &Items, va_t Addr,
                uint64_t Size, va_t Except) {
  auto It = Items.upper_bound(Addr);
  if (It != Items.begin()) {
    const auto Before = std::prev(It);
    if (Before->first != Except && Before->first + Before->second.Size > Addr)
      return Before->first;
  }
  for (; It != Items.end() && It->first < Addr + Size; ++It)
    if (It->first != Except)
      return It->first;
  return std::nullopt;
}

llvm::json::Array dataItemRows(const Session *S) {
  llvm::json::Array Rows;
  for (const auto &[Addr, Item] : S->DataItems) {
    llvm::json::Object Row{{"addr", vaHex(Addr)},
                           {"kind", Item.Kind},
                           {"size", static_cast<int64_t>(Item.Size)}};
    if (!Item.Encoding.empty())
      Row["encoding"] = Item.Encoding;
    Rows.push_back(std::move(Row));
  }
  return Rows;
}
} // namespace

int neverd_item_set(neverd_session_t Sess, neverd_va_t Addr,
                    const char *RowJson) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return -1;
  }
  auto Parsed = llvm::json::parse(RowJson ? RowJson : "");
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    S->setError("a data item is a JSON object");
    return -1;
  }
  const auto *Row = Parsed->getAsObject();
  if (!Row) {
    S->setError("a data item is a JSON object");
    return -1;
  }
  std::string Error;
  auto Item = parseDataItem(*S, Addr, *Row, Error);
  if (!Item) {
    S->setError(Error);
    return -1;
  }
  // An item replaces the one at its address. Bytes the user undefined give
  // way to it and stay undefined around it; it shares no byte with any other
  // item.
  const va_t End = Addr + Item->Size;
  std::vector<va_t> Carved;
  std::vector<std::pair<va_t, uint64_t>> Pieces;
  auto It = S->DataItems.upper_bound(Addr);
  if (It != S->DataItems.begin() &&
      std::prev(It)->first + std::prev(It)->second.Size > Addr)
    --It;
  for (; It != S->DataItems.end() && It->first < End; ++It) {
    const auto &[At, Other] = *It;
    if (Other.Kind != UndefinedItemKind) {
      if (At == Addr)
        continue;
      S->setError(vaHex(Addr) + " overlaps the item at " + vaHex(At));
      return -1;
    }
    Carved.push_back(At);
    if (At < Addr)
      Pieces.emplace_back(At, Addr - At);
    if (At + Other.Size > End)
      Pieces.emplace_back(End, At + Other.Size - End);
  }
  for (const va_t At : Carved)
    S->DataItems.erase(At);
  for (const auto &[At, Size] : Pieces)
    S->DataItems[At] = {std::string(UndefinedItemKind), Size, {}};
  S->DataItems[Addr] = std::move(*Item);
  return 0;
}

int neverd_item_clear(neverd_session_t Sess, neverd_va_t Addr) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->DataItems.erase(Addr)) {
    S->setError("no data item starts at " + vaHex(Addr));
    return -1;
  }
  return 0;
}

int neverd_operand_format_set(neverd_session_t Sess, neverd_va_t Addr,
                              int Operand, const char *FormatJson) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return -1;
  }
  if (Operand < 0 || Operand >= MaxFormattedOperands) {
    S->setError("an operand index is 0 to 7");
    return -1;
  }
  if (!S->Img.readVA(Addr, 1)) {
    S->setError(vaHex(Addr) + " is not in the image");
    return -1;
  }
  std::optional<Session::OperandFormat> Format;
  if (FormatJson) {
    auto Parsed = llvm::json::parse(FormatJson);
    const auto *Object = Parsed ? Parsed->getAsObject() : nullptr;
    if (!Object) {
      if (!Parsed)
        llvm::consumeError(Parsed.takeError());
      S->setError("an operand format is a JSON object");
      return -1;
    }
    std::string Error;
    Format = parseOperandFormat(*Object, Error);
    if (!Format) {
      S->setError(Error);
      return -1;
    }
  }
  // The listing's own spelling is no format of the user's.
  auto &Operands = S->OperandFormats[Addr];
  if (!Format ||
      (Format->Base == kNumberBase && !Format->Negate && !Format->Invert))
    Operands.erase(static_cast<unsigned>(Operand));
  else
    Operands[static_cast<unsigned>(Operand)] = std::move(*Format);
  if (Operands.empty())
    S->OperandFormats.erase(Addr);
  return 0;
}

const char *neverd_operand_formats_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  return dupStr(jsonToString(llvm::json::Value(operandFormatRows(S))));
}

int neverd_operand_formats_save(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->Loaded)
    return -1;
  ProjectWriteLock Lock(S->FilePath);
  if (!Lock) {
    S->setError("project writer unavailable: " + Lock.error());
    return -1;
  }
  return saveSidecar(S, operandsPath(S), operandFormatRows(S),
                     "operand formats")
             ? 0
             : -1;
}

int neverd_operand_formats_load(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->Loaded)
    return -1;
  const auto Path = operandsPath(S);
  std::error_code EC;
  const bool Exists = std::filesystem::exists(Path, EC);
  if (EC) {
    S->setError("cannot inspect operand formats: " + EC.message());
    return -1;
  }
  std::map<va_t, std::map<unsigned, Session::OperandFormat>> Formats;
  if (Exists) {
    std::ifstream In(Path);
    if (!In.is_open()) {
      S->setError("cannot read operand formats");
      return -1;
    }
    const std::string Content((std::istreambuf_iterator<char>(In)),
                              std::istreambuf_iterator<char>());
    auto Parsed = llvm::json::parse(Content);
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
      S->setError("operand formats sidecar is not valid JSON");
      return -1;
    }
    const auto *Rows = Parsed->getAsArray();
    if (!Rows) {
      S->setError("operand formats sidecar is not an array");
      return -1;
    }
    for (const auto &Value : *Rows) {
      const auto *Row = Value.getAsObject();
      const llvm::json::Value *Address = Row ? Row->get("addr") : nullptr;
      const auto Addr =
          Address ? parsePersistedAddress(*Address) : std::optional<va_t>();
      const auto *Operands = Row ? Row->getArray("operands") : nullptr;
      if (!Addr || !Operands) {
        S->setError("operand formats sidecar has an invalid row");
        return -1;
      }
      for (const auto &Entry : *Operands) {
        const auto *Object = Entry.getAsObject();
        const auto Index =
            Object ? Object->getInteger("operand") : std::nullopt;
        std::string Error;
        auto Format =
            Object ? parseOperandFormat(*Object, Error) : std::nullopt;
        if (!Index || *Index < 0 || *Index >= MaxFormattedOperands || !Format) {
          S->setError("operand formats sidecar has an invalid operand" +
                      (Error.empty() ? std::string() : ": " + Error));
          return -1;
        }
        Formats[*Addr][static_cast<unsigned>(*Index)] = std::move(*Format);
      }
    }
  }
  S->OperandFormats = std::move(Formats);
  return 0;
}

const char *neverd_items_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  return dupStr(jsonToString(llvm::json::Value(dataItemRows(S))));
}

int neverd_items_save(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->Loaded)
    return -1;
  ProjectWriteLock Lock(S->FilePath);
  if (!Lock) {
    S->setError("project writer unavailable: " + Lock.error());
    return -1;
  }
  return saveSidecar(S, itemsPath(S), dataItemRows(S), "data items") ? 0 : -1;
}

int neverd_items_load(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->Loaded)
    return -1;
  const auto Path = itemsPath(S);
  std::error_code EC;
  const bool Exists = std::filesystem::exists(Path, EC);
  if (EC) {
    S->setError("cannot inspect data items: " + EC.message());
    return -1;
  }
  std::map<va_t, Session::DataItem> Items;
  if (Exists) {
    std::ifstream In(Path);
    if (!In.is_open()) {
      S->setError("cannot read data items");
      return -1;
    }
    const std::string Content((std::istreambuf_iterator<char>(In)),
                              std::istreambuf_iterator<char>());
    auto Parsed = llvm::json::parse(Content);
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
      S->setError("data items sidecar is not valid JSON");
      return -1;
    }
    const auto *Rows = Parsed->getAsArray();
    if (!Rows) {
      S->setError("data items sidecar is not an array");
      return -1;
    }
    for (const auto &Value : *Rows) {
      const auto *Row = Value.getAsObject();
      const llvm::json::Value *Address = Row ? Row->get("addr") : nullptr;
      const auto Addr =
          Address ? parsePersistedAddress(*Address) : std::optional<va_t>();
      std::string Error;
      auto Item = Addr ? parseDataItem(*S, *Addr, *Row, Error) : std::nullopt;
      if (!Item || overlappingItem(Items, *Addr, Item->Size, InvalidVA)) {
        S->setError("data items sidecar has an invalid row" +
                    (Error.empty() ? std::string() : ": " + Error));
        return -1;
      }
      Items[*Addr] = std::move(*Item);
    }
  }
  S->DataItems = std::move(Items);
  return 0;
}
