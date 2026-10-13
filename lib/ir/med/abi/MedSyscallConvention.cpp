//===- MedSyscallConvention.cpp - Supervisor call registry ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedSyscallConvention.h"

#include "neverd/loader/ELF/ELFOperatingSystem.h"

namespace neverd {

extern const SyscallConvention LinuxAArch64Syscall;
extern const SyscallConvention LinuxARMSyscall;

namespace {

constexpr const SyscallConvention *LinuxSyscalls[] = {&LinuxAArch64Syscall,
                                                      &LinuxARMSyscall};

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
