//===- X64Syscall.h - Linux syscall ABI facts ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X64SYSCALL_H
#define NEVERD_IR_INTRINSICS_X64SYSCALL_H

#include "neverd/Common.h"

#include <cstdint>
#include <optional>

namespace neverd {

/// Return the number of arguments Linux syscall \p Number reads on \p Target.
/// Unknown numbers have no established arity in this table and must retain
/// all register inputs (LinuxSyscallArguments.def lists the verified ones).
constexpr std::optional<unsigned> linuxSyscallArgumentCount(Arch Target,
                                                            uint64_t Number) {
#define NEVERD_LINUX_SYSCALL_ARGUMENTS(TargetArch, Value, Count)               \
  if (Target == Arch::TargetArch && Number == Value)                           \
    return Count;
#include "neverd/ir/intrinsics/LinuxSyscallArguments.def"
  return std::nullopt;
}

/// Return the number of arguments the specified Linux x86-64 syscall reads.
constexpr std::optional<unsigned>
linuxX64SyscallArgumentCount(uint64_t Number) {
  return linuxSyscallArgumentCount(Arch::X64, Number);
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X64SYSCALL_H
