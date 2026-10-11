//===- MedSyscallConvention.cpp - Supervisor call registry ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedSyscallConvention.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/BinaryFormat/ELF.h"

namespace neverd {

extern const SyscallConvention LinuxAArch64Syscall;
extern const SyscallConvention LinuxARMSyscall;

namespace {

constexpr const SyscallConvention *LinuxSyscalls[] = {&LinuxAArch64Syscall,
                                                      &LinuxARMSyscall};

/// Whether \p Image runs under Linux: an ELF image whose OS ABI byte is the
/// System V default that Linux toolchains write, or GNU/Linux.
bool runsUnderLinux(const BinaryImage &Image) {
  return Image.isELF() && Image.ELFMetadata &&
         (Image.ELFMetadata->OSABI == llvm::ELF::ELFOSABI_NONE ||
          Image.ELFMetadata->OSABI == llvm::ELF::ELFOSABI_GNU);
}

} // namespace

const SyscallConvention *syscallConvention(const BinaryImage &Image,
                                           Arch TheArch, Intrinsic Lifted,
                                           uint64_t Immediate) {
  if (!runsUnderLinux(Image))
    return nullptr;
  for (const SyscallConvention *Convention : LinuxSyscalls)
    if (Convention->TheArch == TheArch && Convention->Lifted == Lifted &&
        Convention->Immediate == Immediate)
      return Convention;
  return nullptr;
}

llvm::ArrayRef<const SyscallConvention *> syscallConventions() {
  return LinuxSyscalls;
}

const SyscallConvention *modeledSyscallConvention(Intrinsic Modeled) {
  for (const SyscallConvention *Convention : LinuxSyscalls)
    if (Convention->Modeled == Modeled)
      return Convention;
  return nullptr;
}

} // namespace neverd
