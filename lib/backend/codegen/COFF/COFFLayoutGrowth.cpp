//===- COFFLayoutGrowth.cpp - PE header storage growth -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve virtual layout while growing the file-backed PE header mapping.
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/COFF/COFFLayoutGrowth.h"

#include "neverd/object/PELayout.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Errc.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace neverd {
namespace {
llvm::Error fail(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "PE header growth: " + Message);
}
} // namespace

llvm::Expected<COFFHeaderGrowth>
ensureCOFFSectionHeaderCapacity(std::vector<uint8_t> &Binary,
                                uint16_t AdditionalSections) {
  using namespace llvm::object;
  auto PE = locatePEHeaders(Binary.data(), Binary.size());
  if (Binary.size() > UINT32_MAX || !PE.valid() || !PE.SectionTable ||
      !PE.NumSections || Binary[0] != 'M' || Binary[1] != 'Z')
    return fail("invalid image headers");
  // The native loader limits images to 96 sections, not UINT16_MAX.
  if (uint32_t(PE.NumSections) + AdditionalSections > 96)
    return fail("section count exceeds the native loader limit");
  const uint64_t Headers = getPESizeOfHeaders(PE);
  const uint64_t Alignment = getPEFileAlignment(PE);
  const uint64_t SectionAlignment = getPESectionAlignment(PE);
  const uint64_t Table = PE.SectionTable - Binary.data();
  const uint64_t TableEnd = Table + PE.NumSections * sizeof(coff_section);
  if (!llvm::isPowerOf2_64(Alignment) || Alignment > 65536 ||
      !llvm::isPowerOf2_64(SectionAlignment) || SectionAlignment < Alignment ||
      (SectionAlignment < 4096 && Alignment != SectionAlignment) ||
      (SectionAlignment >= 4096 && Alignment < 512) || Headers % Alignment ||
      Headers > Binary.size() || TableEnd > Headers ||
      PE.PeOffset < sizeof(dos_header))
    return fail("invalid header extent or alignment");

  uint64_t FirstRVA = UINT32_MAX;
  std::vector<std::pair<uint64_t, uint64_t>> RawRanges;
  struct FilePointer {
    uint64_t Field;
    uint32_t Value;
  };
  std::vector<FilePointer> Pointers;
  auto AddPointer = [&](const llvm::support::ulittle32_t *Field,
                        uint64_t Size) -> llvm::Error {
    const uint32_t Value = *Field;
    if ((!Value && Size) ||
        (Value &&
         !rangeInBounds(Value, std::max<uint64_t>(1, Size), Binary.size())))
      return fail("file pointer is not backed by the input");
    if (Value && Value < Headers && Size > Headers - Value)
      return fail("file range crosses the header boundary");
    Pointers.push_back(
        {static_cast<uint64_t>(reinterpret_cast<const uint8_t *>(Field) -
                               Binary.data()),
         Value});
    return llvm::Error::success();
  };
  auto *Sections = reinterpret_cast<const coff_section *>(PE.SectionTable);
  for (unsigned I = 0; I < PE.NumSections; ++I) {
    const auto &S = Sections[I];
    FirstRVA = std::min<uint64_t>(FirstRVA, S.VirtualAddress);
    if (S.VirtualAddress < Headers || S.VirtualAddress % SectionAlignment ||
        (S.SizeOfRawData &&
         (S.PointerToRawData < Headers || S.PointerToRawData % Alignment ||
          S.SizeOfRawData % Alignment)))
      return fail("section overlaps headers or has invalid alignment");
    if (auto Error = AddPointer(&S.PointerToRawData, S.SizeOfRawData))
      return Error;
    if (S.SizeOfRawData)
      RawRanges.emplace_back(S.PointerToRawData,
                             uint64_t(S.PointerToRawData) + S.SizeOfRawData);
    // Extended relocation counts require an object-file interpretation. They
    // are not silently treated as ordinary image relocation tables.
    if (S.Characteristics & llvm::COFF::IMAGE_SCN_LNK_NRELOC_OVFL)
      return fail("extended COFF relocation table in an image");
    if (auto Error = AddPointer(&S.PointerToRelocations,
                                uint64_t(S.NumberOfRelocations) * 10))
      return Error;
    if (auto Error = AddPointer(&S.PointerToLinenumbers,
                                uint64_t(S.NumberOfLinenumbers) * 6))
      return Error;
  }
  llvm::sort(RawRanges);
  for (size_t I = 1; I < RawRanges.size(); ++I)
    if (RawRanges[I].first < RawRanges[I - 1].second)
      return fail("overlapping section file ranges");

  auto FileOffset = [&](uint32_t RVA,
                        uint32_t Size) -> llvm::Expected<uint64_t> {
    if (!Size)
      return fail("empty metadata range");
    if (RVA < Headers) {
      if (!rangeInBounds(RVA, Size, Headers) || RVA < TableEnd)
        return fail("metadata overlaps or crosses the NT headers");
      return RVA;
    }
    uint64_t Result = 0;
    bool Found = false;
    for (unsigned I = 0; I < PE.NumSections; ++I) {
      const auto &S = Sections[I];
      if (RVA < S.VirtualAddress ||
          !rangeInBounds(
              uint64_t(RVA) - S.VirtualAddress, Size,
              std::min<uint64_t>(
                  S.SizeOfRawData,
                  getPESectionContentSize(S.VirtualSize, S.SizeOfRawData))))
        continue;
      if (Found)
        return fail("ambiguous metadata RVA");
      Found = true;
      Result = uint64_t(S.PointerToRawData) + RVA - S.VirtualAddress;
    }
    if (!Found)
      return fail("metadata RVA has no file-backed owner");
    return Result;
  };

  const uint64_t SymbolBytes = uint64_t(PE.FileHeader->NumberOfSymbols) * 18;
  if (auto Error =
          AddPointer(&PE.FileHeader->PointerToSymbolTable, SymbolBytes))
    return Error;
  if (PE.FileHeader->PointerToSymbolTable) {
    const uint64_t Strings = PE.FileHeader->PointerToSymbolTable + SymbolBytes;
    if (!rangeInBounds(Strings, 4, Binary.size()))
      return fail("truncated COFF string table");
    const uint32_t Size = readLE<uint32_t>(Binary.data() + Strings);
    if (Size < 4 || !rangeInBounds(Strings, Size, Binary.size()) ||
        (PE.FileHeader->PointerToSymbolTable < Headers &&
         Strings + Size > Headers))
      return fail("invalid COFF string table extent");
    const auto *Symbols = reinterpret_cast<const coff_symbol16 *>(
        Binary.data() + PE.FileHeader->PointerToSymbolTable);
    for (uint32_t I = 0; I < PE.FileHeader->NumberOfSymbols;) {
      const auto &S = Symbols[I];
      const uint32_t Count = S.NumberOfAuxSymbols;
      if (Count >= uint32_t(PE.FileHeader->NumberOfSymbols) - I)
        return fail("auxiliary COFF symbols exceed the symbol table");
      if (Count) {
        const COFFSymbolRef Symbol(&S);
        if (Symbol.isFunctionDefinition()) {
          if (Count != 1)
            return fail("unexpected function auxiliary symbol count");
          const auto *Aux =
              reinterpret_cast<const coff_aux_function_definition *>(
                  &Symbols[I + 1]);
          if (auto Error = AddPointer(&Aux->PointerToLinenumber,
                                      Aux->PointerToLinenumber ? 6 : 0))
            return Error;
        } else if (S.StorageClass != llvm::COFF::IMAGE_SYM_CLASS_FILE &&
                   S.StorageClass != llvm::COFF::IMAGE_SYM_CLASS_FUNCTION &&
                   !Symbol.isWeakExternal() && !Symbol.isSectionDefinition() &&
                   !Symbol.isCLRToken()) {
          return fail("unmodeled auxiliary COFF symbol");
        }
      }
      I += 1 + Count;
    }
  }
  for (uint32_t I = 0; I < getPENumberOfRvaAndSizes(PE); ++I) {
    const auto *D = getPEDataDirectory(PE, I);
    if (!D)
      return fail("truncated data-directory array");
    if (!D->Size && !D->RelativeVirtualAddress)
      continue;
    if (!D->Size || !D->RelativeVirtualAddress)
      return fail("inconsistent data directory");
    if (I == llvm::COFF::CERTIFICATE_TABLE) {
      if (auto Error = AddPointer(&D->RelativeVirtualAddress, D->Size))
        return Error;
      continue;
    }
    if (I != llvm::COFF::DEBUG_DIRECTORY)
      continue;
    if (D->Size % sizeof(debug_directory))
      return fail("partial debug-directory entry");
    auto Offset = FileOffset(D->RelativeVirtualAddress, D->Size);
    if (!Offset)
      return Offset.takeError();
    for (uint32_t At = 0; At < D->Size; At += sizeof(debug_directory)) {
      const auto *Debug = reinterpret_cast<const debug_directory *>(
          Binary.data() + *Offset + At);
      if (auto Error = AddPointer(&Debug->PointerToRawData, Debug->SizeOfData))
        return Error;
      if (Debug->AddressOfRawData) {
        auto DataOffset =
            FileOffset(Debug->AddressOfRawData, Debug->SizeOfData);
        if (!DataOffset)
          return DataOffset.takeError();
        if (*DataOffset != Debug->PointerToRawData)
          return fail("debug RVA and file pointer disagree");
      }
    }
  }

  const uint64_t SlotBytes =
      uint64_t(AdditionalSections) * sizeof(coff_section);
  // Even zero bytes can be referenced by nested resource, import or other
  // metadata. Reserve new file storage instead of guessing that bytes beyond
  // the live table are unowned padding. Original header RVAs remain intact.
  const uint64_t NTSize = TableEnd - PE.PeOffset;
  const uint64_t NewPE = AdditionalSections ? Headers : PE.PeOffset;
  const uint64_t NewHeaders =
      std::max(Headers, llvm::alignTo(NewPE + NTSize + SlotBytes, Alignment));
  if (NewHeaders > FirstRVA || NewHeaders > UINT32_MAX ||
      Binary.size() + NewHeaders - Headers > UINT32_MAX)
    return fail("header growth would overlap section RVAs or exceed PE limits");
  COFFHeaderGrowth Result{static_cast<uint32_t>(Headers),
                          static_cast<uint32_t>(NewHeaders), PE.PeOffset,
                          static_cast<uint32_t>(NewPE)};
  if (!AdditionalSections)
    return Result;
  // Low-alignment PE images require raw offsets to equal section RVAs. A
  // file-only move cannot preserve that loader contract.
  if (SectionAlignment < 4096 && Result.addedFileBytes())
    return fail("low-alignment image requires virtual layout relocation");

  auto Candidate = Binary;
  Candidate.insert(Candidate.begin() + Headers, Result.addedFileBytes(), 0);
  if (NewPE != PE.PeOffset) {
    std::copy_n(Binary.begin() + PE.PeOffset, NTSize,
                Candidate.begin() + NewPE);
    reinterpret_cast<dos_header *>(Candidate.data())->AddressOfNewExeHeader =
        NewPE;
  }
  for (const auto &P : Pointers) {
    uint64_t Field = P.Field;
    if (Field >= PE.PeOffset && Field < TableEnd)
      Field += NewPE - PE.PeOffset;
    else if (Field >= Headers)
      Field += Result.addedFileBytes();
    const uint32_t Value =
        P.Value >= Headers ? P.Value + Result.addedFileBytes() : P.Value;
    writeLE<uint32_t>(Candidate.data() + Field, Value);
  }
  auto New = locatePEHeaders(Candidate.data(), Candidate.size());
  if (!New.valid() || !New.SectionTable)
    return fail("grown headers could not be decoded");
  if (New.Is64)
    reinterpret_cast<pe32plus_header *>(New.OptionalHeader)->SizeOfHeaders =
        NewHeaders;
  else
    reinterpret_cast<pe32_header *>(New.OptionalHeader)->SizeOfHeaders =
        NewHeaders;
  clearPEChecksum(New);
  Binary.swap(Candidate);
  return Result;
}

} // namespace neverd
