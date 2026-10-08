//===- WindowsLibraryHost.cpp - Guest host for an input DLL --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcessModules.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#include <array>
#include <cstring>

namespace neverd::emulation::windows_process {
using namespace value;
using namespace llvm::object;

llvm::Expected<Image> makeLibraryHost(Program &P, const Image &Input) {
  if (LibraryHostSize > P.Reads.MappedBytes ||
      P.InputName.size() >= LibraryHostIAT - LibraryHostData)
    return failure(text::ModuleBudget);
  // Leave a fixed-base input's preferred span free. The host contains only
  // relative code/data references and may be placed at either address.
  uint64_t Base = LibraryHostBase;
  if (Base < Input.Base + Input.Size && Input.Base < Base + LibraryHostSize)
    Base = Input.Base >= ImageAlignment + LibraryHostSize
               ? ImageAlignment
               : llvm::alignTo(Input.Base + Input.Size, ImageAlignment);
  if (Base >= UserLimit || LibraryHostSize > UserLimit - Base)
    return failure(text::Layout);

  std::vector<uint8_t> Bytes(LibraryHostSize);
  auto Store = [&](uint64_t Offset, const auto &Record) {
    std::memcpy(Bytes.data() + Offset, &Record, sizeof(Record));
  };
  auto String = [&](uint64_t Offset, llvm::StringRef Text) {
    std::copy(Text.begin(), Text.end(), Bytes.begin() + Offset);
  };
  String(LibraryHostData, P.InputName);
  String(LibraryHostProvider, text::Kernel32);
  Image Out{};
  Out.Architecture = Input.Architecture;
  Out.Base = Out.PreferredBase = Base;
  Out.Size = LibraryHostSize;
  Out.Entry = Base + LibraryHostCode;
  Out.Dependencies.push_back(text::Kernel32);
  constexpr const char *APIs[] = {
#define NEVERD_WINDOWS_LIBRARY_HOST_API(Name) #Name,
#include "WindowsLibraryHost.def"
#undef NEVERD_WINDOWS_LIBRARY_HOST_API
  };
  uint64_t Hint = LibraryHostHints;
  for (size_t I = 0; I < std::size(APIs); ++I) {
    const uint64_t Slot = LibraryHostIAT + I * PointerSize;
    const auto *Service = findService(text::Kernel32, APIs[I]);
    if (!Service)
      return failure(text::Layout);
    const llvm::StringRef Name = APIs[I];
    if (Hint + WideSize + Name.size() + 1 > LibraryHostProvider)
      return failure(text::Layout);
    llvm::support::endian::write64le(Bytes.data() + Slot, Hint);
    llvm::support::endian::write64le(
        Bytes.data() + LibraryHostLookup + I * PointerSize, Hint);
    String(Hint + WideSize, Name);
    Hint = llvm::alignTo(Hint + WideSize + Name.size() + 1, WideSize);
    Out.Imports.push_back(
        {Base + Slot, Service, text::Kernel32, 0, Name.str(), std::nullopt});
  }
  coff_import_directory_table_entry Import{};
  Import.ImportLookupTableRVA = LibraryHostLookup;
  Import.ImportAddressTableRVA = LibraryHostIAT;
  Import.NameRVA = LibraryHostProvider;
  Store(LibraryHostImports, Import);
  const auto Code = Input.Architecture == GuestArchitecture::X64
                        ? llvm::ArrayRef(LibraryHostX64)
                        : llvm::ArrayRef(LibraryHostAArch64);
  std::copy(Code.begin(), Code.end(), Bytes.begin() + LibraryHostCode);

  dos_header DOS{};
  std::copy_n(text::DOSMagic, sizeof(DOS.Magic), DOS.Magic);
  DOS.AddressOfNewExeHeader = sizeof(DOS);
  Store(0, DOS);
  uint64_t Cursor = sizeof(DOS);
  std::copy_n(llvm::COFF::PEMagic, sizeof(llvm::COFF::PEMagic),
              Bytes.begin() + Cursor);
  Cursor += sizeof(llvm::COFF::PEMagic);
  coff_file_header COFF{};
  COFF.Machine = Input.Architecture == GuestArchitecture::X64
                     ? llvm::COFF::IMAGE_FILE_MACHINE_AMD64
                     : llvm::COFF::IMAGE_FILE_MACHINE_ARM64;
  COFF.Characteristics = llvm::COFF::IMAGE_FILE_EXECUTABLE_IMAGE |
                         llvm::COFF::IMAGE_FILE_LARGE_ADDRESS_AWARE |
                         llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED;
  COFF.NumberOfSections = 2;
  COFF.SizeOfOptionalHeader =
      sizeof(pe32plus_header) + MaxDirectories * sizeof(data_directory);
  Store(Cursor, COFF);
  Cursor += sizeof(COFF);
  pe32plus_header PE{};
  PE.Magic = llvm::COFF::PE32Header::PE32_PLUS;
  PE.ImageBase = Base;
  PE.AddressOfEntryPoint = LibraryHostCode;
  PE.BaseOfCode = LibraryHostCode;
  PE.SectionAlignment = PageSize;
  PE.FileAlignment = PageSize;
  PE.SizeOfCode = PageSize;
  PE.SizeOfInitializedData = PageSize;
  PE.SizeOfImage = LibraryHostSize;
  PE.SizeOfHeaders = PageSize;
  PE.MajorOperatingSystemVersion = SystemPEVersion;
  PE.MajorSubsystemVersion = SystemPEVersion;
  PE.Subsystem = llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI;
  PE.NumberOfRvaAndSize = MaxDirectories;
  Store(Cursor, PE);
  Cursor += sizeof(PE);
  data_directory Directory{};
  Directory.RelativeVirtualAddress = LibraryHostImports;
  Directory.Size = 2 * sizeof(Import);
  Store(Cursor + llvm::COFF::IMPORT_TABLE * sizeof(Directory), Directory);
  Cursor += MaxDirectories * sizeof(Directory);
  const char *Sections[] = {text::LibraryHostCodeSection,
                            text::LibraryHostDataSection};
  for (size_t I = 0; I < std::size(Sections); ++I) {
    coff_section Section{};
    const llvm::StringRef Name = Sections[I];
    std::copy(Name.begin(), Name.end(), Section.Name);
    Section.VirtualAddress = (I + 1) * PageSize;
    Section.PointerToRawData = (I + 1) * PageSize;
    Section.VirtualSize = PageSize;
    Section.SizeOfRawData = PageSize;
    Section.Characteristics =
        llvm::COFF::IMAGE_SCN_MEM_READ |
        (I ? llvm::COFF::IMAGE_SCN_MEM_WRITE |
                 llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA
           : llvm::COFF::IMAGE_SCN_MEM_EXECUTE |
                 llvm::COFF::IMAGE_SCN_CNT_CODE);
    Store(Cursor, Section);
    Cursor += sizeof(Section);
  }
  for (uint64_t Offset = 0; Offset < LibraryHostSize; Offset += PageSize) {
    const unsigned Access = Read | UserAccessible |
                            (Offset == LibraryHostCode ? Execute : 0) |
                            (Offset == LibraryHostData ? Write : 0);
    Out.Regions.push_back(
        {Base + Offset,
         Access,
         {Bytes.begin() + Offset, Bytes.begin() + Offset + PageSize},
         PageSize,
         PageSize});
  }
  P.Reads.MappedBytes -= LibraryHostSize;
  return Out;
}
} // namespace neverd::emulation::windows_process
