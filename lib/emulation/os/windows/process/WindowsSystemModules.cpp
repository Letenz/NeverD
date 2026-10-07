//===- WindowsSystemModules.cpp - Resident modeled system PE images ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcessModules.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>

namespace neverd::emulation::windows_process {
namespace {
using namespace value;
using namespace llvm::object;
#include "WindowsSyscallNumbers.inc"
#include "WindowsWineExports.inc"
std::optional<uint32_t> nativeSyscall(llvm::StringRef Name) {
  const auto *First = std::begin(WineSyscalls);
  const auto *Last = std::end(WineSyscalls);
  const auto *Found = std::lower_bound(
      First, Last, Name, [](const auto &Entry, llvm::StringRef Key) {
        return llvm::StringRef(Entry.Name) < Key;
      });
  if (Found == Last || llvm::StringRef(Found->Name) != Name)
    return std::nullopt;
  return Found->Id;
}
constexpr SystemProvider Providers[] = {
#define NEVERD_WINDOWS_SYSTEM_MODULE(Name, Family, Base)                       \
  {text::Name, APIProvider::Family, Base},
#include "WindowsSystemModules.def"
#undef NEVERD_WINDOWS_SYSTEM_MODULE
};
static_assert(std::size(Providers) == SystemModuleCount);

llvm::Expected<Image> makeImage(const SystemProvider &Provider,
                                GuestArchitecture Architecture,
                                ImageReadBudget &Budget, bool Opaque = false) {
  // Names the loader resolves by walking this image, including exports the
  // model does not implement. Calling one of those stops with its name; a
  // missing directory entry would instead be a null pointer in the guest.
  std::vector<llvm::StringRef> Names;
  for (const auto &S : services())
    if (!Opaque && findService(Provider.Name, S.Name) == &S)
      Names.push_back(S.Name);
  if (!Opaque) {
    const llvm::StringRef Module = Provider.Name;
    llvm::ArrayRef<const char *> Extra;
    if (Module == text::NTDLL)
      Extra = WineNtdllExports;
    else if (Module == text::Kernel32)
      Extra = WineKernel32Exports;
    else if (Module == text::KernelBase)
      Extra = WineKernelBaseExports;
    for (const char *N : Extra)
      Names.push_back(N);
  }
  llvm::sort(Names);
  Names.erase(std::unique(Names.begin(), Names.end()), Names.end());
  const uint64_t Count = Names.size();
  const bool Native = Provider.Family == APIProvider::Native && !Opaque;
  const uint32_t Stride = Native ? NativeGateStride : GateStride;
  // Only an opaque module has no modeled export; its directory is empty.
  // The code span has to hold one gate per export.
  if ((!Count && !Opaque) || Count * Stride > SystemImageSize - SystemCodeRVA ||
      SystemImageSize > Budget.MappedBytes)
    return failure(text::SystemImage);
  // Raw offsets equal RVAs. Decode the completed bytes with the same loader
  // that owns original PE export identities; do not build a second name map.
  std::vector<uint8_t> File(SystemImageSize);
  auto Store = [&](uint64_t Offset, const auto &Record) {
    std::memcpy(File.data() + Offset, &Record, sizeof(Record));
  };
  export_directory_table_entry Directory{};
  Directory.OrdinalBase = SystemOrdinalBase;
  Directory.AddressTableEntries = Count;
  Directory.NumberOfNamePointers = Count;
  uint64_t Cursor = SystemExportRVA + sizeof(Directory);
  Directory.ExportAddressTableRVA = Cursor;
  Cursor += Count * DWordSize;
  Directory.NamePointerRVA = Cursor;
  Cursor += Count * DWordSize;
  Directory.OrdinalTableRVA = Cursor;
  Cursor += Count * WideSize;
  auto String = [&](llvm::StringRef Text) -> llvm::Expected<uint32_t> {
    if (Cursor >= SystemCodeRVA || Text.size() + 1 > SystemCodeRVA - Cursor)
      return failure(text::SystemImage);
    const uint32_t RVA = Cursor;
    std::copy(Text.begin(), Text.end(), File.begin() + Cursor);
    Cursor += Text.size() + 1;
    return RVA;
  };
  auto Name = String(Provider.Name);
  if (!Name)
    return Name.takeError();
  Directory.NameRVA = *Name;
  for (size_t I = 0; I < Count; ++I) {
    auto Name = String(Names[I]);
    if (!Name)
      return Name.takeError();
    const uint32_t RVA = SystemCodeRVA + I * Stride;
    llvm::support::endian::write32le(
        File.data() + Directory.ExportAddressTableRVA + I * DWordSize, RVA);
    llvm::support::endian::write32le(
        File.data() + Directory.NamePointerRVA + I * DWordSize, *Name);
    llvm::support::endian::write16le(
        File.data() + Directory.OrdinalTableRVA + I * WideSize, I);
    const Service *Exported = findService(Provider.Name, Names[I]);
    if (Architecture == GuestArchitecture::X64) {
      auto Syscall = Native ? nativeSyscall(Names[I]) : std::nullopt;
      if (Syscall) {
        // mov r10, rcx; mov eax, imm32; syscall; ret; int3 padding.
        // The hook reads 32 bytes and copies whole instructions up to syscall.
        std::array<uint8_t, NativeGateStride> Stub{};
        Stub.fill(0xcc);
        const uint8_t Prefix[] = {0x4c, 0x8b, 0xd1, 0xb8};
        std::copy(std::begin(Prefix), std::end(Prefix), Stub.begin());
        llvm::support::endian::write32le(Stub.data() + 4, *Syscall);
        Stub[NativeSyscallOffset] = 0x0f;
        Stub[NativeSyscallOffset + 1] = 0x05;
        Stub[NativeSyscallOffset + 2] = 0xc3;
        std::copy(Stub.begin(), Stub.end(), File.begin() + RVA);
      } else {
        std::copy(std::begin(X64Service), std::end(X64Service),
                  File.begin() + RVA);
        if (Exported && Exported->Kind == API::RaiseException)
          std::copy(std::begin(X64Return), std::end(X64Return),
                    File.begin() + RVA + sizeof(X64Service));
      }
    } else {
      llvm::support::endian::write32le(File.data() + RVA,
                                       ArmServiceInstruction);
      if (Exported && Exported->Kind == API::RaiseException)
        llvm::support::endian::write32le(File.data() + RVA + DWordSize,
                                         ArmReturnInstruction);
    }
  }
  Store(SystemExportRVA, Directory);
  dos_header DOS{};
  std::copy_n(text::DOSMagic, sizeof(DOS.Magic), DOS.Magic);
  DOS.AddressOfNewExeHeader = sizeof(DOS);
  Store(0, DOS);
  uint64_t Header = sizeof(DOS);
  std::copy_n(llvm::COFF::PEMagic, sizeof(llvm::COFF::PEMagic),
              File.begin() + Header);
  Header += sizeof(llvm::COFF::PEMagic);
  coff_file_header COFF{};
  COFF.Machine = Architecture == GuestArchitecture::X64
                     ? llvm::COFF::IMAGE_FILE_MACHINE_AMD64
                     : llvm::COFF::IMAGE_FILE_MACHINE_ARM64;
  std::array<coff_section, 2> Sections{};
  COFF.NumberOfSections = Sections.size();
  COFF.SizeOfOptionalHeader =
      sizeof(pe32plus_header) + MaxDirectories * sizeof(data_directory);
  COFF.Characteristics = llvm::COFF::IMAGE_FILE_EXECUTABLE_IMAGE |
                         llvm::COFF::IMAGE_FILE_DLL |
                         llvm::COFF::IMAGE_FILE_LARGE_ADDRESS_AWARE;
  Store(Header, COFF);
  Header += sizeof(COFF);
  pe32plus_header PE{};
  PE.Magic = llvm::COFF::PE32Header::PE32_PLUS;
  PE.SizeOfCode = SystemImageSize - SystemCodeRVA;
  PE.SizeOfInitializedData = SystemCodeRVA - SystemExportRVA;
  PE.BaseOfCode = SystemCodeRVA;
  PE.ImageBase = Provider.Base;
  PE.SectionAlignment = PageSize;
  PE.FileAlignment = PageSize;
  PE.MajorOperatingSystemVersion = SystemPEVersion;
  PE.MajorSubsystemVersion = SystemPEVersion;
  PE.SizeOfImage = SystemImageSize;
  PE.SizeOfHeaders = PageSize;
  PE.Subsystem = llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI;
  PE.NumberOfRvaAndSize = MaxDirectories;
  Store(Header, PE);
  Header += sizeof(PE);
  data_directory Export{};
  Export.RelativeVirtualAddress = SystemExportRVA;
  Export.Size = Cursor - SystemExportRVA;
  Store(Header + llvm::COFF::EXPORT_TABLE * sizeof(Export), Export);
  Header += MaxDirectories * sizeof(data_directory);
  for (size_t I = 0; I < Sections.size(); ++I) {
    auto &Section = Sections[I];
    const llvm::StringRef Name =
        I ? text::SystemCodeSection : text::SystemExportSection;
    std::copy(Name.begin(), Name.end(), Section.Name);
    const uint32_t Span =
        I ? SystemImageSize - SystemCodeRVA : SystemCodeRVA - SystemExportRVA;
    Section.VirtualSize = Span;
    Section.SizeOfRawData = Span;
    Section.VirtualAddress = I ? SystemCodeRVA : SystemExportRVA;
    Section.PointerToRawData = I ? SystemCodeRVA : SystemExportRVA;
    Section.Characteristics =
        llvm::COFF::IMAGE_SCN_MEM_READ |
        (I ? llvm::COFF::IMAGE_SCN_CNT_CODE | llvm::COFF::IMAGE_SCN_MEM_EXECUTE
           : llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA);
    Store(Header, Section);
    Header += sizeof(Section);
  }
  if (Header > PageSize)
    return failure(text::SystemImage);
  auto Decoded = readPEProgramExports(
      File, {Budget.MetadataBytes, Budget.Records, MaxName});
  if (!Decoded)
    return Decoded.takeError();
  Budget.MetadataBytes -= Decoded->BytesRead;
  Budget.Records -= Decoded->RecordsRead;
  Budget.MappedBytes -= SystemImageSize;
  Image Out{};
  Out.Architecture = Architecture;
  Out.Base = Provider.Base;
  Out.Size = SystemImageSize;
  Out.Exports = std::move(*Decoded);
  for (uint64_t Offset = 0; Offset < SystemImageSize; Offset += PageSize) {
    const unsigned Rights =
        Read | UserAccessible | (Offset >= SystemCodeRVA ? Execute : 0);
    Out.Regions.push_back(
        {Provider.Base + Offset,
         Rights,
         {File.begin() + Offset, File.begin() + Offset + PageSize},
         PageSize,
         PageSize});
  }
  return Out;
}
} // namespace

llvm::ArrayRef<SystemProvider> systemProviders() { return Providers; }

llvm::Expected<uint64_t> opaqueEntry(Program &P, llvm::StringRef Module,
                                     llvm::StringRef Name,
                                     std::optional<uint16_t> Ordinal) {
  const std::string Key = Module.lower();
  for (const auto &Gate : P.Gates)
    if (!Gate.Target && Gate.Module == Key && Gate.Name == Name &&
        Gate.Ordinal == Ordinal)
      return Gate.Gate;
  if (P.OpaqueEntries == OpaqueGateSize / GateStride)
    return failure(text::ModuleBudget);
  const uint64_t Gate = OpaqueGateBase + P.OpaqueEntries++ * GateStride;
  P.Gates.push_back({0, nullptr, Key, Gate, Name.str(), Ordinal});
  return Gate;
}

llvm::Expected<Image> makeOpaqueImage(Program &P, VirtualMemory &Memory,
                                      llvm::StringRef Name) {
  // Any free image slot identifies the module; the preferred one only keeps
  // the layout stable for a given load order.
  auto Base = Memory.reserveImage(OpaqueModuleBase, SystemImageSize, true);
  if (!Base)
    return Base.takeError();
  const std::string Key = Name.lower();
  auto Image = makeImage({Key.c_str(), APIProvider::Kernel, *Base},
                         P.Modules.front().Loaded.Architecture, P.Reads, true);
  if (!Image)
    return llvm::joinErrors(Image.takeError(), Memory.releaseImage(*Base));
  return Image;
}

llvm::Error prepareSystemModules(Program &P, VirtualMemory &Memory,
                                 const ExecutionBudget &Budget) {
  if (P.Modules.size() != 1)
    return failure(text::Lifetime);
  for (const auto &Provider : Providers) {
    if (!Budget.remainingMicroseconds())
      return failure(text::ModuleTimeout);
    auto Image =
        makeImage(Provider, P.Modules.front().Loaded.Architecture, P.Reads);
    if (!Image)
      return Image.takeError();
    auto Base = Memory.reserveImage(Image->Base, Image->Size, false);
    if (!Base)
      return Base.takeError();
    const size_t Index = P.Modules.size();
    Module M;
    M.Loaded = std::move(*Image);
    M.Generation = P.NextGeneration++;
    M.State = ModuleState::Ready;
    M.Pinned = M.System = true;
    M.ExportMetadata = {{0, PageSize},
                        {SystemExportRVA, SystemCodeRVA - SystemExportRVA}};
    for (size_t I = 0; I < M.Loaded.Exports.Entries.size(); ++I) {
      const auto &Export = M.Loaded.Exports.Entries[I];
      M.Ordinals.emplace(Export.Ordinal, I);
      for (const auto &Name : Export.Names) {
        M.Names.emplace(Name, I);
        const auto *Service = findService(Provider.Name, Name);
        if (P.Gates.size() == MaxImports)
          return failure(text::Service);
        const uint64_t Gate = M.Loaded.Base + Export.RVA;
        // An advertised export the model does not implement still has one
        // address. Executing that address stops and names the export.
        P.Gates.push_back(
            {0, Service, Provider.Name, Gate, Name, std::nullopt});
      }
    }
    P.Slots.emplace(Provider.Name, Index);
    P.Identities.push_back({Provider.Name, M.Loaded.Base, M.Loaded.Size, 0});
    P.Modules.push_back(std::move(M));
    P.LoaderInitializationOrder.push_back(Index);
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation::windows_process
