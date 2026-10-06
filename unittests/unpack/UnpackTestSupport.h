//===- UnpackTestSupport.h - Independent PE observations --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reads fixture images through LLVM's object reader, so expectations do not
/// pass through the unpacking library's own PE code.
///
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_UNPACK_UNPACKTESTSUPPORT_H
#define NEVERD_UNITTESTS_UNPACK_UNPACKTESTSUPPORT_H

#include "gtest/gtest.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/MemoryBuffer.h"

#include <compare>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace neverd::unpack::test {
#define NEVERD_UNPACK_TEST_TEXT(Name, Text) inline constexpr char Name[] = Text;
#define NEVERD_UNPACK_TEST_VALUE(Name, Value)                                  \
  inline constexpr uint64_t Name = Value;
#define NEVERD_UNPACK_TEST_BYTES(Name, ...)                                    \
  inline constexpr uint8_t Name[] = {__VA_ARGS__};
#include "UnpackCases.def"
#undef NEVERD_UNPACK_TEST_BYTES
#undef NEVERD_UNPACK_TEST_VALUE
#undef NEVERD_UNPACK_TEST_TEXT
namespace key {
#define NEVERD_UNPACK_TEST_KEY(Name, Text) inline constexpr char Name[] = Text;
#include "UnpackCases.def"
#undef NEVERD_UNPACK_TEST_KEY
} // namespace key

inline std::filesystem::path fixture(const char *Name) {
  return std::filesystem::path(NEVERD_UNPACK_FIXTURE_DIR) / Name;
}

inline std::vector<uint8_t> readFile(const std::filesystem::path &Path) {
  std::ifstream Stream(Path, std::ios::binary);
  return {std::istreambuf_iterator<char>(Stream),
          std::istreambuf_iterator<char>()};
}

/// One PE32+ file as the system loader would map it.
struct Image {
  struct Section {
    std::string Name;
    uint32_t RVA, VirtualSize, Characteristics, FileOffset, FileSize;
  };
  struct Import {
    std::string Module, Name;
    uint64_t Slot;
    auto operator<=>(const Import &) const = default;
  };
  std::vector<uint8_t> File, Mapped;
  uint64_t Base = 0;
  uint32_t Entry = 0, DLLCharacteristics = 0, FileCharacteristics = 0;
  /// File offsets of the header fields a test may rewrite.
  uint64_t MachineOffset = 0, EntryOffset = 0, SectionTableOffset = 0;
  std::vector<Section> Sections;
  std::set<Import> Imports;
  /// Exported names and the addresses they name.
  std::map<std::string, uint32_t> Exports;
  const Section *section(llvm::StringRef Name) const {
    for (const auto &S : Sections)
      if (S.Name == Name)
        return &S;
    return nullptr;
  }
  /// Bytes that only describe imports or relocations: a recovered image
  /// carries its own description of both.
  std::set<uint32_t> LoaderMetadata;
  llvm::object::data_directory directory(unsigned Index) const {
    return Index < Directories.size() ? Directories[Index]
                                      : llvm::object::data_directory{};
  }
  std::vector<llvm::object::data_directory> Directories;
};

inline Image readImage(std::vector<uint8_t> Bytes) {
  using namespace llvm::object;
  Image Out;
  Out.File = std::move(Bytes);
  const llvm::MemoryBufferRef Buffer(
      llvm::StringRef(reinterpret_cast<const char *>(Out.File.data()),
                      Out.File.size()),
      FixtureBufferName);
  auto Object = COFFObjectFile::create(Buffer);
  if (!Object) {
    ADD_FAILURE() << llvm::toString(Object.takeError());
    return Out;
  }
  const auto &COFF = **Object;
  const auto *PE = COFF.getPE32PlusHeader();
  if (!PE) {
    ADD_FAILURE() << NotPE32Plus;
    return Out;
  }
  const auto At = [&](const void *Field) {
    return uint64_t(static_cast<const uint8_t *>(Field) - Out.File.data());
  };
  Out.MachineOffset = At(&COFF.getCOFFHeader()->Machine);
  Out.EntryOffset = At(&PE->AddressOfEntryPoint);
  Out.Base = PE->ImageBase;
  Out.Entry = PE->AddressOfEntryPoint;
  Out.DLLCharacteristics = PE->DLLCharacteristics;
  Out.FileCharacteristics = COFF.getCOFFHeader()->Characteristics;
  Out.Mapped.assign(PE->SizeOfImage, 0);
  std::copy_n(Out.File.begin(), uint32_t(PE->SizeOfHeaders),
              Out.Mapped.begin());
  for (unsigned I = 0; I < PE->NumberOfRvaAndSize; ++I)
    Out.Directories.push_back(*COFF.getDataDirectory(I));
  for (const auto &Ref : COFF.sections()) {
    const auto *S = COFF.getCOFFSection(Ref);
    if (Out.Sections.empty())
      Out.SectionTableOffset = At(S);
    Out.Sections.push_back(
        {llvm::cantFail(Ref.getName()).str(), S->VirtualAddress, S->VirtualSize,
         S->Characteristics, S->PointerToRawData, S->SizeOfRawData});
    const uint32_t Stored =
        std::min<uint32_t>(S->SizeOfRawData, S->VirtualSize);
    std::copy_n(Out.File.begin() + S->PointerToRawData, Stored,
                Out.Mapped.begin() + S->VirtualAddress);
  }
  for (const auto &Export : COFF.export_directories()) {
    llvm::StringRef Name;
    uint32_t RVA = 0;
    llvm::cantFail(Export.getSymbolName(Name));
    llvm::cantFail(Export.getExportRVA(RVA));
    Out.Exports[Name.str()] = RVA;
  }
  auto Mark = [&](uint32_t RVA, uint32_t Size) {
    for (uint32_t I = 0; I < Size; ++I)
      Out.LoaderMetadata.insert(RVA + I);
  };
  const auto Imports = Out.directory(llvm::COFF::IMPORT_TABLE);
  Mark(Imports.RelativeVirtualAddress, Imports.Size);
  const auto Relocations = Out.directory(llvm::COFF::BASE_RELOCATION_TABLE);
  Mark(Relocations.RelativeVirtualAddress, Relocations.Size);
  for (const auto &Directory : COFF.import_directories()) {
    llvm::StringRef Module;
    uint32_t Lookup = 0, Address = 0;
    const coff_import_directory_table_entry *Entry = nullptr;
    llvm::cantFail(Directory.getName(Module));
    llvm::cantFail(Directory.getImportLookupTableRVA(Lookup));
    llvm::cantFail(Directory.getImportAddressTableRVA(Address));
    llvm::cantFail(Directory.getImportTableEntry(Entry));
    Mark(Entry->NameRVA, Module.size() + 1);
    uint32_t Index = 0;
    for (const auto &Symbol : Directory.imported_symbols()) {
      bool Ordinal = false;
      llvm::cantFail(Symbol.isOrdinal(Ordinal));
      std::string Name;
      if (Ordinal) {
        uint16_t Number = 0;
        llvm::cantFail(Symbol.getOrdinal(Number));
        Name = OrdinalPrefix + std::to_string(Number);
      } else {
        llvm::StringRef Text;
        uint32_t Hint = 0;
        llvm::cantFail(Symbol.getSymbolName(Text));
        llvm::cantFail(Symbol.getHintNameRVA(Hint));
        Name = Text.str();
        Mark(Hint, HintBytes + Text.size() + 1);
      }
      Out.Imports.insert(
          {Module.lower(), Name, Address + Index * PointerBytes});
      // The cell itself holds a loader value in either image.
      Mark(Address + Index * PointerBytes, PointerBytes);
      if (Lookup)
        Mark(Lookup + Index * PointerBytes, PointerBytes);
      ++Index;
    }
  }
  return Out;
}

inline Image readImage(const std::filesystem::path &Path) {
  return readImage(readFile(Path));
}

/// Bytes of \p S that differ between two mapped images, outside the bytes
/// that only describe how \p Original is loaded.
inline uint64_t differingBytes(const Image &Original, const Image &Other,
                               const Image::Section &S) {
  uint64_t Different = 0;
  for (uint32_t RVA = S.RVA; RVA < S.RVA + S.VirtualSize; ++RVA)
    if (!Original.LoaderMetadata.contains(RVA))
      Different += RVA >= Other.Mapped.size() ||
                   Original.Mapped[RVA] != Other.Mapped[RVA];
  return Different;
}

inline void writeFile(const std::filesystem::path &Path,
                      const std::vector<uint8_t> &Bytes) {
  std::ofstream Stream(Path, std::ios::binary);
  Stream.write(reinterpret_cast<const char *>(Bytes.data()),
               std::streamsize(Bytes.size()));
}
} // namespace neverd::unpack::test
#endif
