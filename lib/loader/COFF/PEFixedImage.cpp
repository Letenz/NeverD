//===- PEFixedImage.cpp - Authenticate preferred-base image bytes --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/PEFixedImage.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

namespace neverd {
namespace {

using namespace llvm::COFF;
using namespace llvm::object;
using llvm::support::endian::read16le;
using llvm::support::endian::read32le;
using llvm::support::endian::read64le;

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "PE fixed image: " + Message);
}
llvm::Error unsupported(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "PE fixed image: " + Message);
}
llvm::Error exhausted() {
  return llvm::createStringError(
      std::make_error_code(std::errc::value_too_large),
      "PE fixed image: metadata budget exhausted");
}

struct Range {
  uint64_t Begin = 0;
  uint64_t End = 0;
};

bool overlaps(Range A, Range B) { return A.Begin < B.End && B.Begin < A.End; }

bool disjoint(std::vector<Range> Ranges) {
  llvm::sort(Ranges, [](Range A, Range B) { return A.Begin < B.Begin; });
  for (size_t I = 1; I < Ranges.size(); ++I)
    if (Ranges[I].Begin < Ranges[I - 1].End)
      return false;
  return true;
}

// LLVM's COFF reader initializes some directory-specific structures eagerly.
// Validate the fixed headers and exclude unsupported initialization before
// giving it mutable caller-owned Raw bytes, even if an earlier load succeeded.
llvm::Error preflightFixedHeader(llvm::ArrayRef<uint8_t> Bytes) {
  constexpr size_t DOSSize = sizeof(dos_header);
  constexpr size_t SignatureSize = 4;
  constexpr size_t FixedOptionalSize = sizeof(pe32plus_header);
  constexpr unsigned ReservedDirectory = NUM_DATA_DIRECTORIES;
  constexpr unsigned StandardDirectories = ReservedDirectory + 1;
  if (Bytes.size() < DOSSize || Bytes[0] != 'M' || Bytes[1] != 'Z')
    return invalid("missing or truncated DOS header");
  const uint64_t PE =
      read32le(Bytes.data() + offsetof(dos_header, AddressOfNewExeHeader));
  if (PE < DOSSize || PE > Bytes.size() ||
      SignatureSize + sizeof(coff_file_header) > Bytes.size() - PE)
    return invalid("PE header offset is outside the file");
  if (read32le(Bytes.data() + PE) != 0x00004550)
    return invalid("invalid PE signature");
  const uint64_t COFF = PE + SignatureSize;
  const uint64_t Optional = COFF + sizeof(coff_file_header);
  const uint64_t OptionalSize = read16le(
      Bytes.data() + COFF + offsetof(coff_file_header, SizeOfOptionalHeader));
  if (OptionalSize < FixedOptionalSize ||
      OptionalSize > Bytes.size() - Optional)
    return invalid("missing or truncated PE32+ optional header");
  if (read16le(Bytes.data() + COFF) != IMAGE_FILE_MACHINE_AMD64 ||
      read16le(Bytes.data() + Optional) != PE32Header::PE32_PLUS)
    return unsupported("requires an AMD64 PE32+ header");
  const uint32_t Count = read32le(
      Bytes.data() + Optional + offsetof(pe32plus_header, NumberOfRvaAndSize));
  if (Count > StandardDirectories)
    return unsupported("unknown optional-header directory extension");
  if (uint64_t{Count} * sizeof(data_directory) >
      OptionalSize - FixedOptionalSize)
    return invalid("directory inventory exceeds the optional header");
  const uint64_t Sections = read16le(
      Bytes.data() + COFF + offsetof(coff_file_header, NumberOfSections));
  if (Sections * sizeof(coff_section) > Bytes.size() - Optional - OptionalSize)
    return invalid("truncated PE section table");
  constexpr unsigned ExcludedDirectories[] = {
      TLS_TABLE,          LOAD_CONFIG_TABLE,
      BOUND_IMPORT,       DELAY_IMPORT_DESCRIPTOR,
      CLR_RUNTIME_HEADER, ARCHITECTURE,
      GLOBAL_PTR,         ReservedDirectory};
  for (unsigned Index : ExcludedDirectories) {
    if (Index >= Count)
      continue;
    const auto *Directory = Bytes.data() + Optional + FixedOptionalSize +
                            Index * sizeof(data_directory);
    if (read32le(Directory) || read32le(Directory + sizeof(uint32_t)))
      return unsupported("unmodeled loader-writer directory");
  }
  return llvm::Error::success();
}

struct RawSection {
  uint64_t RVA;
  uint64_t VirtualSize;
  uint64_t Offset;
  uint64_t RawSize;
  uint32_t Characteristics;
  llvm::StringRef Name;
};

/// One bounded RVA authority for relocation parsing and snapshot validation.
/// Reverse raw aliases are rejected too: two virtual owners cannot designate
/// the same file occurrence, including a PE/section header occurrence.
class RawImage {
  llvm::ArrayRef<uint8_t> Bytes;
  const COFFObjectFile &Object;
  PEFixedImageLimits Limits;
  uint64_t WorkBytes = 0;
  uint64_t Records = 0;

public:
  uint64_t Base;
  uint64_t HeaderSize = 0;
  std::vector<RawSection> Sections;
  std::vector<size_t> ByRVA;

  RawImage(const COFFObjectFile &Object, PEFixedImageLimits Limits)
      : Bytes(llvm::arrayRefFromStringRef(Object.getData())), Object(Object),
        Limits(Limits), Base(Object.getImageBase()) {}

  llvm::Error account(uint64_t Size, uint64_t Count = 1) {
    if (Size > Limits.MaxBytes - WorkBytes ||
        Count > Limits.MaxRecords - Records)
      return exhausted();
    WorkBytes += Size;
    Records += Count;
    return llvm::Error::success();
  }

  llvm::Error initialize(bool AuthenticateNames = false) {
    if (const auto *Header = Object.getPE32PlusHeader())
      HeaderSize = Header->SizeOfHeaders;
    else if (const auto *Header = Object.getPE32Header())
      HeaderSize = Header->SizeOfHeaders;
    else
      return unsupported("requires a linked PE image");
    if (!HeaderSize || HeaderSize > Bytes.size() ||
        HeaderSize > InvalidVA - Base)
      return invalid("invalid header extent");
    const auto *COFFHeader = Object.getCOFFHeader();
    if (!COFFHeader)
      return invalid("missing linked-image COFF header");
    const uint64_t COFFOffset =
        reinterpret_cast<const uint8_t *>(COFFHeader) - Bytes.data();
    const uint64_t TableEnd =
        COFFOffset + sizeof(coff_file_header) +
        COFFHeader->SizeOfOptionalHeader +
        uint64_t{Object.getNumberOfSections()} * sizeof(coff_section);
    if (TableEnd > HeaderSize)
      return invalid("PE section table exceeds the header extent");
    if (auto Error = account(HeaderSize))
      return Error;
    std::vector<Range> Virtual{{0, HeaderSize}}, Raw{{0, HeaderSize}};
    for (const auto &Ref : Object.sections()) {
      if (auto Error = account(sizeof(coff_section)))
        return Error;
      const auto *S = Object.getCOFFSection(Ref);
      llvm::StringRef Name;
      if (AuthenticateNames) {
        auto ParsedName = Object.getSectionName(S);
        if (!ParsedName)
          return ParsedName.takeError();
        Name = *ParsedName;
        if (auto Error = account(Name.size(), 0))
          return Error;
      }
      const RawSection Section{S->VirtualAddress,   S->VirtualSize,
                               S->PointerToRawData, S->SizeOfRawData,
                               S->Characteristics,  Name};
      if (Section.RVA + Section.VirtualSize > (uint64_t{1} << 32) ||
          Section.RVA + Section.VirtualSize > InvalidVA - Base ||
          Section.Offset + Section.RawSize > Bytes.size())
        return invalid("section extent is outside its address space");
      if (Section.VirtualSize)
        Virtual.push_back({Section.RVA, Section.RVA + Section.VirtualSize});
      if (Section.RawSize)
        Raw.push_back({Section.Offset, Section.Offset + Section.RawSize});
      Sections.push_back(Section);
      if (Section.VirtualSize)
        ByRVA.push_back(Sections.size() - 1);
    }
    if (!disjoint(Virtual) || !disjoint(Raw))
      return invalid("overlapping virtual or raw section owners");
    llvm::sort(ByRVA, [&](size_t A, size_t B) {
      return Sections[A].RVA < Sections[B].RVA;
    });
    return llvm::Error::success();
  }

  const RawSection *owner(uint64_t RVA, uint64_t Size) const {
    if (!Size || RVA >= (uint64_t{1} << 32) || Size > (uint64_t{1} << 32) - RVA)
      return nullptr;
    const auto It = std::upper_bound(
        ByRVA.begin(), ByRVA.end(), RVA,
        [&](uint64_t Address, size_t I) { return Address < Sections[I].RVA; });
    if (It == ByRVA.begin())
      return nullptr;
    const auto &S = Sections[*std::prev(It)];
    const uint64_t Offset = RVA - S.RVA;
    if (Size > S.VirtualSize || Offset > S.VirtualSize - Size ||
        Size > S.RawSize || Offset > S.RawSize - Size)
      return nullptr;
    return &S;
  }

  llvm::Expected<llvm::ArrayRef<uint8_t>> read(uint64_t RVA, uint64_t Size) {
    const auto *S = owner(RVA, Size);
    if (!S)
      return invalid("metadata or fixup is not uniquely file-backed");
    if (auto Error = account(Size))
      return std::move(Error);
    return Bytes.slice(S->Offset + RVA - S->RVA, Size);
  }

  llvm::Expected<Range> directory(unsigned Index) {
    const auto *D = Object.getDataDirectory(Index);
    if (!D)
      return Range{};
    const uint64_t RVA = D->RelativeVirtualAddress, Size = D->Size;
    if ((RVA == 0) != (Size == 0))
      return invalid("incomplete directory address/size pair");
    if (!RVA)
      return Range{};
    if (!owner(RVA, Size))
      return invalid("directory is not uniquely file-backed");
    return Range{RVA, RVA + Size};
  }

  llvm::Expected<Range> string(uint64_t RVA, uint64_t Prefix = 0) {
    const auto *S = owner(RVA, Prefix + 1);
    if (!S)
      return invalid("import name is not file-backed");
    const uint64_t Available =
        std::min(S->RawSize, S->VirtualSize) - (RVA - S->RVA);
    for (uint64_t I = Prefix; I < Available; ++I) {
      if (auto Error = account(1, 0))
        return std::move(Error);
      if (!Bytes[S->Offset + RVA - S->RVA + I]) {
        if (I == Prefix)
          return invalid("empty import name");
        return Range{RVA, RVA + I + 1};
      }
    }
    return invalid("unterminated import name");
  }
};

struct RelocationInventory {
  Range Directory;
  std::vector<BaseRelocation> Entries;
  std::vector<Range> Fields;
  bool OnlyDir64 = true;
};

llvm::Expected<RelocationInventory> relocations(RawImage &Raw) {
  RelocationInventory Result;
  auto Directory = Raw.directory(BASE_RELOCATION_TABLE);
  if (!Directory)
    return Directory.takeError();
  Result.Directory = *Directory;
  if (!Directory->End)
    return Result;
  if (Directory->Begin % alignof(uint32_t))
    return invalid("unaligned relocation block address");
  auto Data = Raw.read(Directory->Begin, Directory->End - Directory->Begin);
  if (!Data)
    return Data.takeError();
  constexpr size_t HeaderSize = sizeof(coff_base_reloc_block_header);
  size_t Offset = 0;
  while (Offset != Data->size()) {
    if (Data->size() - Offset < HeaderSize || Offset % alignof(uint32_t))
      return invalid("truncated or unaligned relocation block");
    const uint32_t Page = read32le(Data->data() + Offset);
    const uint32_t Size = read32le(Data->data() + Offset + sizeof(uint32_t));
    if ((Page & kBaseRelocOffsetMask) || Size < HeaderSize ||
        Size > Data->size() - Offset || Size % alignof(uint32_t))
      return invalid("invalid relocation block extent");
    for (size_t I = HeaderSize; I < Size; I += sizeof(uint16_t)) {
      if (auto Error = Raw.account(0))
        return std::move(Error);
      const uint16_t Word = read16le(Data->data() + Offset + I);
      const uint8_t Type = Word >> kBaseRelocOffsetBits;
      if (Type == IMAGE_REL_BASED_ABSOLUTE)
        continue;
      const uint64_t RVA = uint64_t{Page} + (Word & kBaseRelocOffsetMask);
      if (RVA >= (uint64_t{1} << 32) || RVA > InvalidVA - Raw.Base)
        return invalid("relocation address overflows");
      uint32_t Width = 0;
      switch (Type) {
      case IMAGE_REL_BASED_DIR64:
        Width = sizeof(uint64_t);
        break;
      case IMAGE_REL_BASED_HIGHLOW:
        Width = sizeof(uint32_t);
        break;
      case IMAGE_REL_BASED_HIGHADJ:
        I += sizeof(uint16_t);
        if (I == Size)
          return invalid("HIGHADJ relocation lacks its payload word");
        [[fallthrough]];
      case IMAGE_REL_BASED_HIGH:
      case IMAGE_REL_BASED_LOW:
        Width = sizeof(uint16_t);
        break;
      default:
        // Preserve unsupported format records without guessing their width.
        break;
      }
      Result.OnlyDir64 &= Type == IMAGE_REL_BASED_DIR64;
      if (Width) {
        auto Field = Raw.read(RVA, Width);
        if (!Field)
          return Field.takeError();
        Result.Fields.push_back({RVA, RVA + Width});
      }
      Result.Entries.push_back({Raw.Base + RVA, Type});
    }
    Offset += Size;
  }
  if (!disjoint(Result.Fields))
    return invalid("overlapping relocation fields");
  return Result;
}

struct ProtectedRead {
  Range Bytes;
  std::optional<uint64_t> InPlaceWriter;
};

struct ImportInventory {
  std::vector<Range> Writes;
  Range IAT;
};

llvm::Expected<ImportInventory> imports(RawImage &Raw,
                                        Range RelocationDirectory) {
  ImportInventory Result;
  auto IATRange = Raw.directory(IAT);
  if (!IATRange)
    return IATRange.takeError();
  Result.IAT = *IATRange;
  auto Directory = Raw.directory(IMPORT_TABLE);
  if (!Directory)
    return Directory.takeError();
  if (!Directory->End) {
    if (IATRange->End)
      return unsupported("IAT directory without a complete import table");
    return Result;
  }
  std::vector<ProtectedRead> Reads{{{0, Raw.HeaderSize}, std::nullopt}};
  if (RelocationDirectory.End)
    Reads.push_back({RelocationDirectory, std::nullopt});
  bool Terminated = false;
  constexpr size_t DescriptorSize = sizeof(coff_import_directory_table_entry);
  for (uint64_t At = Directory->Begin; Directory->End - At >= DescriptorSize;
       At += DescriptorSize) {
    auto Descriptor = Raw.read(At, DescriptorSize);
    if (!Descriptor)
      return Descriptor.takeError();
    Reads.push_back({{At, At + DescriptorSize}, std::nullopt});
    if (llvm::all_of(*Descriptor, [](uint8_t B) { return B == 0; })) {
      Terminated = true;
      break;
    }
    const uint32_t Original = read32le(Descriptor->data());
    const uint32_t Name = read32le(Descriptor->data() + 12);
    const uint32_t First = read32le(Descriptor->data() + 16);
    if (!Name || !First)
      return invalid("import descriptor lacks its name or IAT");
    auto Module = Raw.string(Name);
    if (!Module)
      return Module.takeError();
    Reads.push_back({*Module, std::nullopt});
    const uint64_t Lookup = Original ? Original : First;
    for (uint64_t Offset = 0;; Offset += sizeof(uint64_t)) {
      auto Word = Raw.read(Lookup + Offset, sizeof(uint64_t));
      if (!Word)
        return Word.takeError();
      const uint64_t Value = read64le(Word->data());
      const uint64_t Destination = uint64_t{First} + Offset;
      Reads.push_back({{Lookup + Offset, Lookup + Offset + sizeof(uint64_t)},
                       !Original && Value ? std::optional<uint64_t>(Destination)
                                          : std::nullopt});
      if (!Value)
        break;
      auto Slot = Raw.read(Destination, sizeof(uint64_t));
      if (!Slot)
        return Slot.takeError();
      const Range Write{Destination, Destination + sizeof(uint64_t)};
      if (IATRange->End &&
          (Write.Begin < IATRange->Begin || Write.End > IATRange->End))
        return invalid("import destination lies outside the IAT directory");
      Result.Writes.push_back(Write);
      if (Value & (uint64_t{1} << 63)) {
        if (Value & ~(uint64_t{1} << 63 | uint64_t{0xffff}))
          return invalid("invalid ordinal import encoding");
      } else {
        if (Value > UINT32_MAX)
          return invalid("import name RVA overflows");
        auto Symbol = Raw.string(Value, sizeof(uint16_t));
        if (!Symbol)
          return Symbol.takeError();
        Reads.push_back({*Symbol, std::nullopt});
      }
    }
  }
  if (!Terminated)
    return invalid("import directory lacks its terminating descriptor");
  llvm::sort(Result.Writes, [](Range A, Range B) { return A.Begin < B.Begin; });
  if (!disjoint(Result.Writes))
    return invalid("overlapping import destination slots");
  for (const auto &Read : Reads) {
    auto It = std::lower_bound(
        Result.Writes.begin(), Result.Writes.end(), Read.Bytes.Begin,
        [](Range Write, uint64_t Begin) { return Write.End <= Begin; });
    for (; It != Result.Writes.end() && It->Begin < Read.Bytes.End; ++It)
      if (!Read.InPlaceWriter || *Read.InPlaceWriter != It->Begin ||
          Read.Bytes.Begin != It->Begin || Read.Bytes.End != It->End)
        return invalid("import writes overlap loader metadata");
  }
  return Result;
}

} // namespace

llvm::Error coff_loader::parseBaseRelocations(const COFFObjectFile &Object,
                                              BinaryImage &Image,
                                              uint64_t ImageBase) {
  if (!Image.LoadOnlyFunctionEntries.empty() || Image.IsRelocatable)
    return llvm::Error::success();
  const auto *Directory = Object.getDataDirectory(BASE_RELOCATION_TABLE);
  if (!Directory || (!Directory->RelativeVirtualAddress && !Directory->Size))
    return llvm::Error::success();
  // Ordinary loading is bounded by the already-owned file. Do not impose the
  // optional fixed-snapshot view's smaller analysis budget on every PE load.
  const uint64_t FileSize = Object.getData().size();
  const auto Scaled = [&](uint64_t Factor) {
    return FileSize > UINT64_MAX / Factor ? UINT64_MAX : FileSize * Factor;
  };
  RawImage Raw(Object, {.MaxBytes = Scaled(8), .MaxRecords = Scaled(2)});
  if (ImageBase != Raw.Base)
    return invalid("base relocation mapping differs from its header");
  if (auto Error = Raw.initialize())
    return Error;
  auto Inventory = relocations(Raw);
  if (!Inventory)
    return Inventory.takeError();
  std::vector<AbsolutePointerRelocation> Pointers;
  for (const auto &R : Inventory->Entries) {
    const uint32_t Width = Image.getPointerSize();
    if ((Width == 8 && R.Type == IMAGE_REL_BASED_DIR64) ||
        (Width == 4 && R.Type == IMAGE_REL_BASED_HIGHLOW))
      if (const auto *Bytes = Image.readVA(R.Address, Width))
        Pointers.push_back({R.Address, readPtr(Bytes, Width == 8)});
  }
  Image.BaseRelocations = std::move(Inventory->Entries);
  recordAbsolutePointerRelocations(Image, Pointers);
  return llvm::Error::success();
}

struct PEFixedImageView::Storage {
  const BinaryImage *Image = nullptr;
  std::vector<size_t> Mappings;
  std::vector<Range> Writes;
  Range IAT;
  std::string Digest;
};

PEFixedImageView::PEFixedImageView(std::shared_ptr<const Storage> Data)
    : Data(std::move(Data)) {}

llvm::Expected<PEFixedImageView>
PEFixedImageView::create(const BinaryImage &Image,
                         const PEFixedImageLimits &Limits) {
  if (Image.Format != BinaryFormat::COFF || Image.Arch != Arch::X64 ||
      Image.Bits != Bitness::Bits64 || Image.IsRelocatable ||
      Image.Raw.empty() || !Image.LoadOnlyFunctionEntries.empty())
    return unsupported("requires complete linked x64 PE loader bytes");
  if (Image.Raw.size() > Limits.MaxBytes)
    return exhausted();
  if (auto Error = preflightFixedHeader(Image.Raw))
    return std::move(Error);
  const llvm::StringRef RawBytes(
      reinterpret_cast<const char *>(Image.Raw.data()), Image.Raw.size());
  auto Object = COFFObjectFile::create(llvm::MemoryBufferRef(RawBytes, {}));
  if (!Object)
    return Object.takeError();
  const auto *Header = (*Object)->getPE32PlusHeader();
  if (!Header || (*Object)->getMachine() != IMAGE_FILE_MACHINE_AMD64 ||
      (*Object)->getImageBase() != Image.Base)
    return invalid("mapped image and preferred-base header disagree");
  RawImage Raw(**Object, Limits);
  if (auto Error = Raw.initialize(true))
    return std::move(Error);
  if (auto Error = Raw.account(Image.Raw.size(), 0))
    return std::move(Error);
  if (!Image.Relocations.empty() || !Image.DyldBindSlots.empty() ||
      !Image.RuntimeCallablePointerSlots.empty())
    return unsupported("additional relocation or runtime writer metadata");
  if (Raw.Sections.size() != Image.Segments.size() ||
      Raw.Sections.size() != Image.Sections.size())
    return invalid("mapped sections differ from the PE section inventory");
  for (size_t I = 0; I < Raw.Sections.size(); ++I) {
    const auto &R = Raw.Sections[I];
    const auto &M = Image.Segments[I];
    const auto &S = Image.Sections[I];
    const auto Flags = coffFlagsToNd(R.Characteristics);
    const auto Matches = [&](const auto &Mapping) {
      return Mapping.VA == Image.Base + R.RVA &&
             Mapping.Size == R.VirtualSize && Mapping.FileOff == R.Offset &&
             Mapping.FileSz == R.RawSize && Mapping.Flags == Flags &&
             Mapping.Name == R.Name;
    };
    if (!Matches(M) || !Matches(S) || S.Type != R.Characteristics ||
        M.ReadOnlyAfterRelocations)
      return invalid("mapped section identity or permissions changed");
    const uint64_t FileBytes = std::min(R.RawSize, R.VirtualSize);
    if (M.Data.size() < FileBytes || S.Data.size() < FileBytes)
      return invalid("incomplete mapped section bytes");
    if (auto Error = Raw.account(M.Data.size() + S.Data.size(), 0))
      return std::move(Error);
    const auto MatchesBytes = [&](llvm::ArrayRef<uint8_t> Bytes) {
      if (Bytes.size() > std::max(R.RawSize, R.VirtualSize))
        return false;
      for (size_t J = 0; J < Bytes.size(); ++J)
        if (Bytes[J] != (J < R.RawSize ? Image.Raw[R.Offset + J] : 0))
          return false;
      return true;
    };
    if (!MatchesBytes(M.Data) || !MatchesBytes(S.Data))
      return invalid("mapped bytes differ from the fixed file snapshot");
  }
  auto Relocations = relocations(Raw);
  if (!Relocations)
    return Relocations.takeError();
  if (!Relocations->OnlyDir64)
    return unsupported("only DIR64 fields have a fixed-base contract");
  if (Relocations->Entries.size() != Image.BaseRelocations.size())
    return invalid("base relocation inventory changed");
  for (size_t I = 0; I < Image.BaseRelocations.size(); ++I)
    if (Image.BaseRelocations[I].Address != Relocations->Entries[I].Address ||
        Image.BaseRelocations[I].Type != Relocations->Entries[I].Type)
      return invalid("base relocation entry changed");
  auto Imports = imports(Raw, Relocations->Directory);
  if (!Imports)
    return Imports.takeError();
  for (size_t Count : {Image.Imports.size(), Image.ImportStorageSlots.size(),
                       Image.ImportPtrSlots.size()})
    if (auto Error = Raw.account(0, Count))
      return std::move(Error);
  std::set<va_t> ImportSlots;
  for (Range Write : Imports->Writes)
    ImportSlots.insert(Image.Base + Write.Begin);
  const auto CheckSlot = [&](va_t Address) {
    return ImportSlots.contains(Address);
  };
  for (const auto &Imported : Image.Imports)
    if (!CheckSlot(Imported.IATAddr))
      return invalid("import metadata contains an uncertified writer");
  for (const auto &[Address, Unused] : Image.ImportStorageSlots)
    if (!CheckSlot(Address))
      return invalid("import storage contains an uncertified writer");
  for (const auto &[Address, Unused] : Image.ImportPtrSlots)
    if (!CheckSlot(Address))
      return invalid("import pointer contains an uncertified writer");
  if (!Image.ConflictingImportStorageSlots.empty())
    return invalid("conflicting import storage metadata");

  // Recompute the natural slot/operand byproducts with the same semantic
  // owner as ordinary loading. Extra model provenance must not disappear
  // merely because the provider can now authenticate raw DIR64 bytes.
  for (size_t Count :
       {Image.CodePtrRelocSlots.size(), Image.DataPtrRelocSlots.size(),
        Image.DataPtrRelocTargetOwners.size(),
        Image.CodeAddressRelocOperands.size(),
        Image.DataAddressRelocOperands.size()})
    if (auto Error = Raw.account(0, Count))
      return std::move(Error);
  if (!Image.RelCodeRelocSlots.empty() || !Image.RelDataPtrRelocSlots.empty())
    return invalid("relative provenance conflicts with the DIR64 inventory");
  std::set<va_t> CodeSlots, DataSlots;
  std::map<va_t, va_t> DataOwners;
  std::map<va_t, RelocatedAddressField> CodeOperands, DataOperands;
  for (const auto &R : Relocations->Entries) {
    if (ImportSlots.contains(R.Address))
      continue;
    const uint64_t Target = read64le(Image.readVA(R.Address, sizeof(uint64_t)));
    const auto Effect = decideAbsolutePointerRelocation(
        Image, R.Address, Target, InvalidVA, [&](va_t Address) {
          return classifyPointerRelocationAddress(Image, Address);
        });
    using Kind = AbsolutePointerRelocationEffect::Kind;
    switch (Effect.What) {
    case Kind::CodeOperandToCode:
      CodeOperands.emplace(R.Address, Effect.field(Image));
      break;
    case Kind::CodeOperandToData:
      DataOperands.emplace(R.Address, Effect.field(Image));
      break;
    case Kind::DataSlotToCode:
      CodeSlots.insert(R.Address);
      break;
    case Kind::DataSlotToData:
      DataSlots.insert(R.Address);
      DataOwners.emplace(R.Address, Effect.TargetOwnerBegin);
      break;
    default:
      break;
    }
  }
  const auto SameFields = [](const auto &A, const auto &B) {
    if (A.size() != B.size())
      return false;
    return std::equal(
        A.begin(), A.end(), B.begin(), [](const auto &X, const auto &Y) {
          return X.first == Y.first &&
                 X.second.EncodedValue == Y.second.EncodedValue &&
                 X.second.TargetVA == Y.second.TargetVA &&
                 X.second.Width == Y.second.Width &&
                 X.second.TargetOwnerVA == Y.second.TargetOwnerVA &&
                 X.second.PCRelativeFromInstructionEnd ==
                     Y.second.PCRelativeFromInstructionEnd &&
                 X.second.Kind == Y.second.Kind;
        });
  };
  if (CodeSlots != Image.CodePtrRelocSlots ||
      DataSlots != Image.DataPtrRelocSlots ||
      DataOwners != Image.DataPtrRelocTargetOwners ||
      !SameFields(CodeOperands, Image.CodeAddressRelocOperands) ||
      !SameFields(DataOperands, Image.DataAddressRelocOperands))
    return invalid(
        "derived relocation provenance differs from the raw inventory");

  auto Data = std::make_shared<Storage>();
  Data->Image = &Image;
  Data->Mappings = Raw.ByRVA;
  Data->Writes = std::move(Imports->Writes);
  Data->IAT = Imports->IAT;
  // Raw binds all headers, directory declarations, thunk/relocation records,
  // complete fixup values and file bytes. Mapping validation above proves its
  // identity with the current image. The bounded parser owns derived ranges.
  llvm::SHA256 Hash;
  Hash.update("neverd-pe-preferred-base-snapshot-v1");
  Hash.update(Image.Raw);
  uint8_t Budget[16];
  llvm::support::endian::write64le(Budget, Limits.MaxBytes);
  llvm::support::endian::write64le(Budget + 8, Limits.MaxRecords);
  Hash.update(llvm::ArrayRef<uint8_t>(Budget));
  Data->Digest = llvm::toHex(Hash.final());
  return PEFixedImageView(std::move(Data));
}

std::optional<llvm::ArrayRef<uint8_t>>
PEFixedImageView::read(va_t Address, uint32_t Bytes, bool Executable) const {
  const auto &Image = *Data->Image;
  if (!Bytes || Address < Image.Base || Address > InvalidVA - Bytes)
    return std::nullopt;
  const Range Requested{Address - Image.Base, Address - Image.Base + Bytes};
  if (overlaps(Requested, Data->IAT))
    return std::nullopt;
  const auto Write = std::lower_bound(
      Data->Writes.begin(), Data->Writes.end(), Requested.Begin,
      [](Range R, uint64_t Begin) { return R.End <= Begin; });
  if (Write != Data->Writes.end() && overlaps(*Write, Requested))
    return std::nullopt;
  const auto It = std::upper_bound(
      Data->Mappings.begin(), Data->Mappings.end(), Address,
      [&](va_t A, size_t I) { return A < Image.Segments[I].VA; });
  if (It == Data->Mappings.begin())
    return std::nullopt;
  const auto &S = Image.Segments[*std::prev(It)];
  const uint64_t Offset = Address - S.VA;
  if (!S.isReadable() || S.isWritable() || (Executable && !S.isExecutable()) ||
      Bytes > S.Size || Offset > S.Size - Bytes || Bytes > S.FileSz ||
      Offset > S.FileSz - Bytes || Bytes > S.Data.size() ||
      Offset > S.Data.size() - Bytes)
    return std::nullopt;
  return llvm::ArrayRef<uint8_t>(S.Data).slice(Offset, Bytes);
}

const std::string &PEFixedImageView::digest() const { return Data->Digest; }

} // namespace neverd
