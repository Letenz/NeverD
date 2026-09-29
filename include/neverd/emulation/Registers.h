//===- Registers.h - Typed guest register access -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_REGISTERS_H
#define NEVERD_EMULATION_REGISTERS_H
#include "neverd/emulation/ExecutionBackend.h"

#include <array>
#include <cstdint>
namespace neverd::emulation {
using RegisterValue = std::array<uint64_t, 2>;
enum class CPURegister {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend) Arch##Name,
#define NEVERD_VECTOR_REGISTER(Arch, Index, Backend) Arch##V##Index,
#include "neverd/emulation/Registers.def"
#undef NEVERD_SCALAR_REGISTER
#undef NEVERD_VECTOR_REGISTER
  Invalid
};
enum class X64Register {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  NEVERD_REGISTER_##Arch(Name)
#define NEVERD_REGISTER_X64(Name) Name,
#define NEVERD_REGISTER_AArch64(Name)
#include "neverd/emulation/Registers.def"
#undef NEVERD_REGISTER_AArch64
#undef NEVERD_REGISTER_X64
#undef NEVERD_SCALAR_REGISTER
};
enum class AArch64Register {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  NEVERD_REGISTER_##Arch(Name)
#define NEVERD_REGISTER_X64(Name)
#define NEVERD_REGISTER_AArch64(Name) Name,
#include "neverd/emulation/Registers.def"
#undef NEVERD_REGISTER_AArch64
#undef NEVERD_REGISTER_X64
#undef NEVERD_SCALAR_REGISTER
};
CPURegister cpuRegister(X64Register Register);
CPURegister cpuRegister(AArch64Register Register);
CPURegister vectorRegister(GuestArchitecture Architecture, unsigned Index);
bool registerMatches(CPURegister Register, GuestArchitecture Architecture);
unsigned registerWidth(CPURegister Register);
} // namespace neverd::emulation
#endif
