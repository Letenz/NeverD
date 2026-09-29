//===- LinuxProcessLayout.cpp - ELF facts and process-entry policy -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxProcess.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/loader/BinaryImageModel.h"

#include "llvm/BinaryFormat/ELF.h"

namespace neverd::emulation::linux_model {
llvm::Expected<ProcessLayout> processLayout(const BinaryImage &Image) {
  using namespace llvm::ELF;
  if (!Image.isELF() || Image.Bits != Bitness::Bits64 || !Image.ELFMetadata ||
      Image.ELFMetadata->Type != ET_EXEC ||
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
  std::optional<uint64_t> TableAddress, DeclaredAddress;
  std::optional<bool> ExecutableStack;
  for (const auto &Header : Metadata.ProgramHeaders) {
    if (Header.Type == PT_INTERP || Header.Type == PT_DYNAMIC ||
        Header.Type == PT_TLS)
      return failure(Dynamic);
    if (Header.Type == PT_GNU_STACK) {
      const bool Executable = bool(Header.Flags & PF_X);
      if (ExecutableStack && *ExecutableStack != Executable)
        return failure(ProgramHeaders);
      ExecutableStack = Executable;
    }
    if (Header.Type == PT_PHDR) {
      if (DeclaredAddress || Header.FileSize != TableSize ||
          Header.MemorySize != TableSize || Header.FileOffset != Offset)
        return failure(ProgramHeaders);
      DeclaredAddress = Header.VirtualAddress;
    }
    if (Header.Type != PT_LOAD || !Header.MemorySize)
      continue;
    if (Header.VirtualAddress < MinimumAddress ||
        Header.VirtualAddress >= UserLimit ||
        Header.MemorySize > UserLimit - Header.VirtualAddress)
      return failure(AddressRange);
    if (Header.FileSize > Header.MemorySize ||
        Header.FileOffset > Image.Raw.size() ||
        Header.FileSize > Image.Raw.size() - Header.FileOffset ||
        Header.VirtualAddress % PageSize != Header.FileOffset % PageSize ||
        (Header.Alignment > 1 && ((Header.Alignment & (Header.Alignment - 1)) ||
                                  Header.VirtualAddress % Header.Alignment !=
                                      Header.FileOffset % Header.Alignment)))
      return failure(ProgramHeaders);
    if (Offset >= Header.FileOffset &&
        Offset - Header.FileOffset <= Header.FileSize &&
        TableSize <= Header.FileSize - (Offset - Header.FileOffset)) {
      const uint64_t Address =
          Header.VirtualAddress + (Offset - Header.FileOffset);
      if (TableAddress && *TableAddress != Address)
        return failure(ProgramHeaders);
      TableAddress = Address;
    }
  }
  if (!TableAddress || (DeclaredAddress && DeclaredAddress != TableAddress))
    return failure(ProgramHeaders);
  auto Calls = IntegerABI::get(X64 ? IntegerCallingConvention::SysVAMD64
                                   : IntegerCallingConvention::AAPCS64);
  if (!Calls)
    return Calls.takeError();
  return ProcessLayout{Architecture, *Calls,
                       UserLimit,    *TableAddress,
                       PageSize,     ExecutableStack.value_or(false)};
}
} // namespace neverd::emulation::linux_model
