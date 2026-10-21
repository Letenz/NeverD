//===- MapDebugContextBase.cpp - Shared COFF MAP parser -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared COFF /MAP parser used by both MSVCMapLoader and LLDMapLoader.
///
//===----------------------------------------------------------------------===//

#include "neverd/debug/MapDebugContextBase.h"

#include "MapParsers.h"

#include "neverd/debug/Limits.h"
#include "neverd/debug/MapFile.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>

namespace neverd {

//===----------------------------------------------------------------------===//
// MapDebugContextBase virtual method implementations
//===----------------------------------------------------------------------===//

std::optional<FunctionSym>
MapDebugContextBase::resolveFunction(va_t Addr) const {
  auto It = Functions.find(Addr);
  if (It != Functions.end())
    return It->second;
  auto LB = Functions.lower_bound(Addr);
  if (LB != Functions.begin()) {
    --LB;
    if (LB->second.contains(Addr))
      return LB->second;
  }
  return std::nullopt;
}

std::optional<VariableSym> MapDebugContextBase::resolveVariable(va_t,
                                                                int64_t) const {
  return std::nullopt;
}

std::optional<TypeSym> MapDebugContextBase::resolveType(uint64_t) const {
  return std::nullopt;
}

std::optional<SourceLoc> MapDebugContextBase::sourceLocation(va_t Addr) const {
  if (SourceLocations.empty())
    return std::nullopt;
  auto It = SourceLocations.upper_bound(Addr);
  if (It == SourceLocations.begin())
    return std::nullopt;
  --It;
  if (It->first == Addr ||
      (Addr - It->first) <
          ::neverd::limits::debug::kMaxNearestMapSymbolDistance)
    return It->second;
  return std::nullopt;
}

std::vector<DataObjectSym> MapDebugContextBase::allDataObjects() const {
  std::vector<DataObjectSym> Result;
  if (!Loaded)
    return Result;
  Result.reserve(DataObjects.size());
  for (const auto &[_, Obj] : DataObjects)
    Result.push_back(Obj);
  return Result;
}

std::vector<FunctionSym> MapDebugContextBase::allFunctions() const {
  std::vector<FunctionSym> Result;
  if (!Loaded)
    return Result;
  Result.reserve(Functions.size());
  for (const auto &[_, FS] : Functions)
    Result.push_back(FS);
  std::sort(Result.begin(), Result.end(),
            [](const FunctionSym &A, const FunctionSym &B) {
              return A.Addr < B.Addr;
            });
  return Result;
}

bool MapDebugContextBase::hasInfo() const { return Loaded; }

bool MapDebugContextBase::isCOFFMapHeader(llvm::StringRef Line) {
  llvm::StringRef Trimmed = Line.trim();
  return Trimmed.starts_with("Address") && Trimmed.contains("Publics by Value");
}

bool MapDebugContextBase::parseSegOffset(llvm::StringRef Token, uint16_t &Seg,
                                         uint64_t &Offset) {
  auto ColonPos = Token.find(':');
  if (ColonPos == llvm::StringRef::npos)
    return false;
  unsigned SegVal;
  if (Token.substr(0, ColonPos).getAsInteger(16, SegVal) || SegVal > 0xFFFFu)
    return false;
  // IDA writes the offsets of a 64-bit database in sixteen digits.
  uint64_t OffVal;
  if (Token.substr(ColonPos + 1).getAsInteger(16, OffVal))
    return false;
  Seg = static_cast<uint16_t>(SegVal);
  Offset = OffVal;
  return true;
}

void MapDebugContextBase::inferFunctionSizes() {
  if (Functions.size() < 2)
    return;
  for (auto It = Functions.begin(); It != Functions.end(); ++It) {
    if (It->second.Size != 0)
      continue;
    auto Next = std::next(It);
    if (Next == Functions.end())
      break;
    uint64_t Gap = Next->first - It->first;
    if (Gap > 0 && Gap <= ::neverd::limits::debug::kMaxMapFunctionGapBytes)
      It->second.Size = Gap;
  }
}

namespace {

struct SectionEntry {
  uint16_t Seg;
  uint64_t Offset;
  uint32_t Length;
  std::string Name;
  std::string Class;
};

bool isCodeSegment(const std::vector<SectionEntry> &Sections, uint16_t Seg) {
  for (auto &S : Sections) {
    if (S.Seg == Seg)
      return S.Class == "CODE";
  }
  return Seg == 1;
}

va_t resolveAddress(const std::vector<SectionEntry> &Sections, uint16_t Seg,
                    uint64_t Offset, uint64_t ImageBase) {
  if (Seg == 0)
    return 0;
  for (auto &S : Sections) {
    if (S.Seg == Seg)
      return ImageBase + S.Offset + Offset;
  }
  return ImageBase + Offset;
}

/// A public as a segmented MAP states it, before the image places it.
struct PublicRow {
  uint16_t Seg = 0;
  uint64_t Offset = 0;
  std::string Name;
  /// link.exe's Rva+Base column: the address at the MAP's image base.
  std::optional<uint64_t> RvaBase;
  bool FuncFlag = false;
};

/// The ways the segment:offset of a public without an Rva+Base column reads
/// as an address.  link.exe numbers a PE's sections from one, and IDA does so
/// for the segments of a PE it loaded; Borland's section table starts each
/// segment at its address; IDA states the address itself for an image whose
/// segments are flat, such as an ELF or Mach-O one.
enum class SegmentReading { ImageSection, TableStart, Flat, Legacy };

std::optional<va_t> readAddress(SegmentReading Reading, const PublicRow &Row,
                                const std::vector<SectionEntry> &Sections,
                                const MapImageLayout &Layout,
                                uint64_t MapBase) {
  switch (Reading) {
  case SegmentReading::ImageSection:
    if (Row.Seg == 0 || Row.Seg > Layout.Sections.size())
      return std::nullopt;
    return Layout.Sections[Row.Seg - 1].Address + Row.Offset;
  case SegmentReading::TableStart:
    for (const SectionEntry &S : Sections)
      if (S.Seg == Row.Seg)
        return S.Offset + Row.Offset;
    return std::nullopt;
  case SegmentReading::Flat:
    return Row.Offset;
  case SegmentReading::Legacy:
    return resolveAddress(Sections, Row.Seg, Row.Offset, MapBase);
  }
  return std::nullopt;
}

/// The reading that places the most of \p Rows in the image, preferring the
/// image's own section numbering for a PE.
SegmentReading chooseReading(const std::vector<PublicRow> &Rows,
                             const std::vector<SectionEntry> &Sections,
                             const MapImageLayout &Layout, uint64_t MapBase) {
  const bool PE = Layout.Format == BinaryFormat::COFF;
  const SegmentReading Order[] = {
      PE ? SegmentReading::ImageSection : SegmentReading::Flat,
      SegmentReading::TableStart,
      PE ? SegmentReading::Flat : SegmentReading::ImageSection,
      SegmentReading::Legacy};
  SegmentReading Best = SegmentReading::Legacy;
  size_t BestPlaced = 0;
  for (SegmentReading Reading : Order) {
    size_t Placed = 0;
    for (const PublicRow &Row : Rows)
      if (!Row.RvaBase)
        if (auto VA = readAddress(Reading, Row, Sections, Layout, MapBase);
            VA && Layout.isMapped(*VA))
          ++Placed;
    if (Placed > BestPlaced) {
      Best = Reading;
      BestPlaced = Placed;
    }
  }
  return Best;
}

} // anonymous namespace

bool isAutomaticNameOf(llvm::StringRef Name, va_t Addr) {
  static constexpr llvm::StringLiteral Prefixes[] = {
#define NEVERD_MAP_AUTOMATIC_NAME_PREFIX(Prefix) Prefix,
#include "neverd/debug/MapAutomaticNames.def"
  };
  constexpr size_t MaxHexDigits = 16;
  for (llvm::StringRef Prefix : Prefixes) {
    llvm::StringRef Digits = Name;
    uint64_t Value = 0;
    if (Digits.consume_front(Prefix) && !Digits.empty() &&
        Digits.size() <= MaxHexDigits && !Digits.getAsInteger(16, Value))
      return Value == Addr;
  }
  return false;
}

void MapDebugContextBase::parseCOFFMapContent(
    llvm::StringRef Content, std::map<va_t, FunctionSym> &Functions,
    uint64_t ImageBase, std::map<va_t, DataObjectSym> *DataObjects,
    const MapImageLayout *Layout) {

  enum class ParseState { Header, Sections, Symbols, Done };
  ParseState State = ParseState::Header;
  uint64_t PreferredBase = ImageBase;
  bool StatesPreferredBase = false;
  // link.exe follows each public with its address; IDA and Borland do not,
  // and IDA's demangled names hold spaces.
  bool HasRvaColumn = false;
  std::vector<SectionEntry> Sections;
  std::vector<PublicRow> Rows;

  const auto addFunction = [&](va_t VA, std::string Name) {
    if (Functions.find(VA) != Functions.end())
      return;
    FunctionSym FS;
    FS.Name = std::move(Name);
    FS.Addr = VA;
    Functions[VA] = std::move(FS);
  };
  const auto addData = [&](va_t VA, std::string Name) {
    if (!DataObjects || DataObjects->find(VA) != DataObjects->end())
      return;
    DataObjectSym Obj;
    Obj.Name = std::move(Name);
    Obj.Addr = VA;
    Obj.Size = 0;
    Obj.IsBuffer = false;
    (*DataObjects)[VA] = std::move(Obj);
  };

  llvm::StringRef Remaining = Content;
  while (!Remaining.empty()) {
    auto [Line, Rest] = Remaining.split('\n');
    Remaining = Rest;

    llvm::StringRef Trimmed = Line.trim();
    if (Trimmed.empty())
      continue;

    if (Trimmed.starts_with("Preferred load address is")) {
      llvm::StringRef AddrStr = Trimmed.substr(25).trim();
      unsigned long long Val;
      if (!AddrStr.getAsInteger(16, Val)) {
        PreferredBase = Val;
        StatesPreferredBase = true;
      }
      continue;
    }

    if (Trimmed.starts_with("Start         Length") ||
        Trimmed.starts_with("Start ")) {
      State = ParseState::Sections;
      continue;
    }

    if (MapDebugContextBase::isCOFFMapHeader(Trimmed)) {
      State = ParseState::Symbols;
      HasRvaColumn = Trimmed.contains("Rva+Base");
      continue;
    }

    if (Trimmed.starts_with("Static symbols")) {
      State = ParseState::Symbols;
      continue;
    }

    if (Trimmed.starts_with("entry point at") ||
        Trimmed.starts_with("Program entry point at")) {
      State = ParseState::Done;
      continue;
    }

    if (State == ParseState::Sections) {
      llvm::SmallVector<llvm::StringRef, 8> Tokens;
      Trimmed.split(Tokens, ' ', -1, false);
      if (Tokens.size() >= 3) {
        uint16_t Seg;
        uint64_t Offset;
        if (parseSegOffset(Tokens[0], Seg, Offset)) {
          SectionEntry Entry;
          Entry.Seg = Seg;
          Entry.Offset = Offset;
          llvm::StringRef LenStr = Tokens[1];
          if (LenStr.ends_with("H") || LenStr.ends_with("h"))
            LenStr = LenStr.drop_back();
          unsigned Len;
          Entry.Length = !LenStr.getAsInteger(16, Len) ? Len : 0;
          if (Tokens.size() > 2)
            Entry.Name = Tokens[2].str();
          if (Tokens.size() > 3)
            Entry.Class = Tokens[3].str();
          Sections.push_back(std::move(Entry));
        }
      }
    } else if (State == ParseState::Symbols) {
      llvm::SmallVector<llvm::StringRef, 8> Tokens;
      Trimmed.split(Tokens, ' ', -1, false);
      if (Tokens.size() < 2)
        continue;

      uint16_t Seg;
      uint64_t Offset;
      if (!parseSegOffset(Tokens[0], Seg, Offset))
        continue;

      bool HasFuncFlag = std::find(Tokens.begin() + 2, Tokens.end(),
                                   llvm::StringRef("f")) != Tokens.end();

      std::optional<uint64_t> RvaBase;
      if (Tokens.size() >= 3 && (!Layout || HasRvaColumn)) {
        llvm::StringRef RvaStr = Tokens[2];
        if (RvaStr.starts_with("0x") || RvaStr.starts_with("0X"))
          RvaStr = RvaStr.drop_front(2);
        unsigned long long Value;
        if (!RvaStr.getAsInteger(16, Value) && Value != 0)
          RvaBase = Value;
      }

      if (!Layout) {
        std::string Name = Tokens[1].str();
        va_t VA = RvaBase
                      ? static_cast<va_t>(*RvaBase)
                      : resolveAddress(Sections, Seg, Offset, PreferredBase);
        if (VA == 0 || Name.empty())
          continue;
        if (HasFuncFlag || isCodeSegment(Sections, Seg))
          addFunction(VA, std::move(Name));
        else
          addData(VA, std::move(Name));
        continue;
      }

      PublicRow Row;
      Row.Seg = Seg;
      Row.Offset = Offset;
      Row.RvaBase = RvaBase;
      Row.FuncFlag = HasFuncFlag;
      Row.Name = HasRvaColumn
                     ? Tokens[1].str()
                     : Trimmed.drop_front(Tokens[0].size()).trim().str();
      if (!Row.Name.empty() && Seg != 0)
        Rows.push_back(std::move(Row));
    }
  }

  if (!Layout)
    return;

  // The image the MAP is read against places what the MAP does not: the
  // address of a public without one, and which addresses hold code.
  const uint64_t MapBase = StatesPreferredBase ? PreferredBase : Layout->Base;
  const SegmentReading Reading =
      chooseReading(Rows, Sections, *Layout, MapBase);
  const bool SectionsKnown = !Layout->Sections.empty();
  for (PublicRow &Row : Rows) {
    std::optional<va_t> VA;
    if (Row.RvaBase) {
      // An image rebased since the link moves its names with it.
      VA = static_cast<va_t>(*Row.RvaBase - MapBase + Layout->Base);
    } else {
      VA = readAddress(Reading, Row, Sections, *Layout, MapBase);
    }
    if (!VA || *VA == 0 || isAutomaticNameOf(Row.Name, *VA))
      continue;
    const bool Code =
        SectionsKnown ? Layout->isCode(*VA) : isCodeSegment(Sections, Row.Seg);
    if (Row.FuncFlag || Code)
      addFunction(*VA, std::move(Row.Name));
    else if (!SectionsKnown || Layout->isMapped(*VA))
      addData(*VA, std::move(Row.Name));
  }
}

void MapDebugContextBase::parseCOFFMapLineNumbers(
    llvm::StringRef Content, std::map<va_t, SourceLoc> &Locations,
    uint64_t ImageBase, const MapImageLayout *Layout) {

  std::string CurrentFile;
  bool InLineNumbers = false;

  std::vector<SectionEntry> Sections;
  llvm::StringRef Remaining = Content;
  while (!Remaining.empty()) {
    auto [Line, Rest] = Remaining.split('\n');
    Remaining = Rest;

    llvm::StringRef Trimmed = Line.trim();
    if (Trimmed.empty()) {
      if (InLineNumbers)
        InLineNumbers = false;
      continue;
    }

    if (Trimmed.starts_with("Start         Length") ||
        Trimmed.starts_with("Start ")) {
      llvm::StringRef SRemaining = Rest;
      while (!SRemaining.empty()) {
        auto [SLine, SRest] = SRemaining.split('\n');
        SRemaining = SRest;
        llvm::StringRef STrimmed = SLine.trim();
        if (STrimmed.empty())
          break;
        llvm::SmallVector<llvm::StringRef, 8> Tokens;
        STrimmed.split(Tokens, ' ', -1, false);
        if (Tokens.size() >= 3) {
          uint16_t Seg;
          uint64_t Offset;
          if (parseSegOffset(Tokens[0], Seg, Offset)) {
            SectionEntry Entry;
            Entry.Seg = Seg;
            Entry.Offset = Offset;
            Sections.push_back(std::move(Entry));
          }
        }
      }
      continue;
    }

    if (Trimmed.starts_with("Line numbers for ")) {
      llvm::StringRef FileInfo = Trimmed.substr(17);
      auto ParenPos = FileInfo.find('(');
      if (ParenPos != llvm::StringRef::npos)
        CurrentFile = FileInfo.substr(0, ParenPos).trim().str();
      else
        CurrentFile = FileInfo.str();
      InLineNumbers = true;
      continue;
    }

    if (!InLineNumbers || CurrentFile.empty())
      continue;

    llvm::SmallVector<llvm::StringRef, 16> Tokens;
    Trimmed.split(Tokens, ' ', -1, false);

    for (size_t I = 0; I + 1 < Tokens.size(); I += 2) {
      unsigned LineNum;
      if (Tokens[I].getAsInteger(10, LineNum))
        continue;

      uint16_t Seg;
      uint64_t Offset;
      if (!parseSegOffset(Tokens[I + 1], Seg, Offset))
        continue;

      // link.exe numbers the segments as the image numbers its sections.
      va_t VA = 0;
      if (Layout && Seg != 0 && Seg <= Layout->Sections.size())
        VA = Layout->Sections[Seg - 1].Address + Offset;
      else if (!Layout)
        VA = resolveAddress(Sections, Seg, Offset, ImageBase);
      if (VA == 0)
        continue;

      if (Locations.find(VA) == Locations.end()) {
        SourceLoc Loc;
        Loc.File = CurrentFile;
        Loc.Line = LineNum;
        Locations[VA] = std::move(Loc);
      }
    }
  }
}

} // namespace neverd
