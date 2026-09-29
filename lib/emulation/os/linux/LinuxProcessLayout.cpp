//===- LinuxProcessLayout.cpp - ELF facts and process-entry policy -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxProcess.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/loader/ELF/ELFProgramMetadata.h"

#include "llvm/BinaryFormat/ELF.h"

namespace neverd::emulation::linux_model {
namespace {
bool validAlignment(const ELFProgramHeader &Header) {
  return Header.Alignment <= 1 ||
         (!(Header.Alignment & (Header.Alignment - 1)) &&
          Header.VirtualAddress % Header.Alignment ==
              Header.FileOffset % Header.Alignment);
}
bool hasReadableFileTemplate(const ELFImageMetadata &Metadata,
                             const ELFProgramHeader &Header) {
  if (!Header.FileSize)
    return true;
  for (const auto &Load : Metadata.ProgramHeaders) {
    if (Load.Type != llvm::ELF::PT_LOAD || !(Load.Flags & llvm::ELF::PF_R) ||
        Header.FileOffset < Load.FileOffset ||
        Header.VirtualAddress < Load.VirtualAddress)
      continue;
    const uint64_t Delta = Header.FileOffset - Load.FileOffset;
    if (Delta == Header.VirtualAddress - Load.VirtualAddress &&
        Delta <= Load.FileSize && Header.FileSize <= Load.FileSize - Delta)
      return true;
  }
  return false;
}
llvm::Error validateDynamicDependencies(const BinaryImage &Image) {
  auto Entries = readELFProgramDynamicTable(Image);
  if (!Entries)
    return Entries.takeError();
  for (const auto &Entry : *Entries) {
    switch (Entry.Tag) {
#define NEVERD_LINUX_DYNAMIC_DEPENDENCY(Name, Tag) case Tag:
#include "LinuxValues.def"
#undef NEVERD_LINUX_DYNAMIC_DEPENDENCY
      return failure(Dynamic);
    default:
      break;
    }
  }
  return llvm::Error::success();
}
} // namespace
llvm::Expected<ProcessLayout> processLayout(const BinaryImage &Image) {
  using namespace llvm::ELF;
  if (!Image.isELF() || Image.Bits != Bitness::Bits64 || !Image.ELFMetadata ||
      (Image.ELFMetadata->Type != ET_EXEC &&
       Image.ELFMetadata->Type != ET_DYN) ||
      (Image.Arch != Arch::X64 && Image.Arch != Arch::AArch64))
    return failure(linux_model::Image);
  const auto &Metadata = *Image.ELFMetadata;
  if ((Metadata.OSABI != ELFOSABI_NONE && Metadata.OSABI != ELFOSABI_LINUX) ||
      Metadata.ABIVersion || Metadata.Flags)
    return failure(ABI);
  const bool X64 = Image.Arch == Arch::X64;
  const auto Architecture =
      X64 ? GuestArchitecture::X64 : GuestArchitecture::AArch64;
  auto Capabilities =
      executionCapabilities(X64 ? ExecutionContract::CheckedUserX64
                                : ExecutionContract::CheckedUserAArch64,
                            Architecture);
  if (!Capabilities)
    return Capabilities.takeError();
  const uint64_t PageSize = Capabilities->PageSize;
  const uint64_t UserLimit = X64 ? UserLimitX64 : UserLimitARM64;
  if (Metadata.ProgramHeaderEntrySize != sizeof(Elf64_Phdr) ||
      Metadata.ProgramHeaders.empty() ||
      Metadata.ProgramHeaders.size() > Image.Raw.size() / sizeof(Elf64_Phdr))
    return failure(ProgramHeaders);
  const uint64_t TableSize =
      Metadata.ProgramHeaders.size() * sizeof(Elf64_Phdr);
  const uint64_t Offset = Metadata.ProgramHeaderFileOffset;
  if (Offset > Image.Raw.size() || TableSize > Image.Raw.size() - Offset)
    return failure(ProgramHeaders);
  uint64_t LoadBias = Metadata.Type == ET_DYN ? StaticPIEBias : 0;
  if (LoadBias)
    for (const auto &Header : Metadata.ProgramHeaders)
      if (Header.Type == PT_LOAD && Header.MemorySize) {
        if (!validAlignment(Header))
          return failure(ProgramHeaders);
        LoadBias = std::max(LoadBias, Header.Alignment);
      }
  if (LoadBias >= UserLimit || Image.Entry >= UserLimit - LoadBias ||
      Image.Base >= UserLimit - LoadBias)
    return failure(AddressRange);
  auto UserRange = [&](uint64_t Address, uint64_t Size) {
    return Address < UserLimit - LoadBias &&
           Address + LoadBias >= MinimumAddress &&
           Size <= UserLimit - LoadBias - Address;
  };
  std::optional<uint64_t> TableAddress, DeclaredAddress;
  std::optional<bool> ExecutableStack;
  bool HasTLS = false;
  for (const auto &Header : Metadata.ProgramHeaders) {
    if (Header.Type == PT_INTERP)
      return failure(Dynamic);
    if (Header.Type == PT_DYNAMIC) {
      if (!Header.FileSize || Header.FileSize > Header.MemorySize ||
          Header.FileSize > DynamicEntryLimit * sizeof(Elf64_Dyn) ||
          !UserRange(Header.VirtualAddress, Header.MemorySize) ||
          !validAlignment(Header) || !hasReadableFileTemplate(Metadata, Header))
        return failure(DynamicLayout);
    }
    if (Header.Type == PT_TLS) {
      if (HasTLS || !Header.MemorySize || Header.FileSize > Header.MemorySize ||
          Header.FileOffset > Image.Raw.size() ||
          Header.FileSize > Image.Raw.size() - Header.FileOffset ||
          !UserRange(Header.VirtualAddress, Header.MemorySize) ||
          !validAlignment(Header) || !hasReadableFileTemplate(Metadata, Header))
        return failure(TLS);
      HasTLS = true;
      // The ELF template is ordinary image data. Guest startup initializes
      // each TLS block and installs its thread pointer, as on Linux; the OS
      // model does not guess a libc-specific TCB, DTV or dynamic TLS layout.
    }
    if (Header.Type == PT_GNU_STACK) {
      const bool Executable = bool(Header.Flags & PF_X);
      if (ExecutableStack && *ExecutableStack != Executable)
        return failure(ProgramHeaders);
      ExecutableStack = Executable;
    }
    if (Header.Type == PT_PHDR) {
      if (DeclaredAddress || Header.FileSize != TableSize ||
          Header.MemorySize != TableSize || Header.FileOffset != Offset ||
          !UserRange(Header.VirtualAddress, TableSize))
        return failure(ProgramHeaders);
      DeclaredAddress = Header.VirtualAddress + LoadBias;
    }
    if (Header.Type != PT_LOAD || !Header.MemorySize)
      continue;
    if (!UserRange(Header.VirtualAddress, Header.MemorySize))
      return failure(AddressRange);
    if (Header.FileSize > Header.MemorySize ||
        Header.FileOffset > Image.Raw.size() ||
        Header.FileSize > Image.Raw.size() - Header.FileOffset ||
        Header.VirtualAddress % PageSize != Header.FileOffset % PageSize ||
        !validAlignment(Header))
      return failure(ProgramHeaders);
    if (Offset >= Header.FileOffset &&
        Offset - Header.FileOffset <= Header.FileSize &&
        TableSize <= Header.FileSize - (Offset - Header.FileOffset)) {
      const uint64_t Address =
          Header.VirtualAddress + LoadBias + (Offset - Header.FileOffset);
      if (TableAddress && *TableAddress != Address)
        return failure(ProgramHeaders);
      TableAddress = Address;
    }
  }
  if (!TableAddress || (DeclaredAddress && DeclaredAddress != TableAddress))
    return failure(ProgramHeaders);
  if (auto E = validateDynamicDependencies(Image))
    return E;
  auto Calls = IntegerABI::get(X64 ? IntegerCallingConvention::SysVAMD64
                                   : IntegerCallingConvention::AAPCS64);
  if (!Calls)
    return Calls.takeError();
  return ProcessLayout{Architecture,
                       *Calls,
                       UserLimit,
                       *TableAddress,
                       PageSize,
                       LoadBias,
                       ExecutableStack.value_or(false)};
}
} // namespace neverd::emulation::linux_model
