//===- UnicornMachine.cpp - Portable checked-contract execution ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/aarch64/AArch64Machine.h"
#include "../../arch/x86_64/X64Machine.h"
#include "../../core/ExecutionDiagnostics.h"
#include "../MachineFactories.h"
#include "UnicornArchitecture.h"

#include <unicorn/arm64.h>
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>

namespace neverd::emulation {
namespace {
llvm::Error check(uc_err Status) {
  if (Status == UC_ERR_OK)
    return llvm::Error::success();
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 uc_strerror(Status));
}
/// The checked architecture owns admission and observations. This transport
/// executes one admitted instruction over the same authoritative RAM pages.
class UnicornStepper {
public:
  uc_engine *Engine = nullptr;
  PhysicalMemory &Memory;
  explicit UnicornStepper(PhysicalMemory &Memory) : Memory(Memory) {}
  ~UnicornStepper() {
    if (Engine)
      uc_close(Engine);
  }
  llvm::Error initialize(uc_arch Arch, uc_mode Mode) {
    if (auto E = check(uc_open(Arch, Mode, &Engine)))
      return E;
    if (auto E = check(uc_ctl_tlb_mode(Engine, UC_TLB_VIRTUAL)))
      return E;
    return initializeUnicornArchitecture(
        Engine, Arch == UC_ARCH_X86 ? GuestArchitecture::X64
                                    : GuestArchitecture::AArch64);
  }
  llvm::Error synchronize() {
    if (Generation == Memory.mappingGeneration())
      return llvm::Error::success();
    for (const auto &[Address, P] : Mapped)
      if (auto E = check(uc_mem_unmap(Engine, Address, memory::PageSize)))
        return E;
    Mapped.clear();
    for (const auto &[Address, P] : Memory.mappings()) {
      if (auto E =
              check(uc_mem_map_ptr(Engine, Address, memory::PageSize,
                                   P.Permissions, Memory.data() + P.Physical)))
        return E;
      Mapped.emplace(Address, P);
    }
    Generation = Memory.mappingGeneration();
    return llvm::Error::success();
  }
  llvm::Error run(uint64_t PC) {
    // Host backing writes bypass Unicorn's code-write invalidation. Discard
    // translated blocks before entering so aliases and self-modifying code
    // observe current authoritative bytes.
    if (auto E = check(uc_ctl_flush_tb(Engine)))
      return E;
    return check(uc_emu_start(Engine, PC, 0, 0, 1));
  }

private:
  uint64_t Generation = 0;
  std::map<uint64_t, PhysicalMemory::Page> Mapped;
};
class UnicornX64Machine final : public X64Machine {
public:
  UnicornStepper CPU;
  explicit UnicornX64Machine(PhysicalMemory &Memory) : CPU(Memory) {}
  llvm::Error step(X64MachineState &State, uint64_t,
                   std::chrono::steady_clock::time_point) override {
    if (auto E = CPU.synchronize())
      return E;
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  if (auto E = check(uc_reg_write(CPU.Engine, registerID(X64Register::Name),   \
                                  &State.reg(X64Register::Name))))             \
    return E;
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
    if (auto E =
            check(uc_reg_write(CPU.Engine, UC_X86_REG_GS_BASE, &State.GSBase)))
      return E;
    if (auto E = CPU.run(State.reg(X64Register::PC)))
      return E;
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  if (auto E = check(uc_reg_read(CPU.Engine, registerID(X64Register::Name),    \
                                 &State.reg(X64Register::Name))))              \
    return E;
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
    return llvm::Error::success();
  }

private:
  static int registerID(X64Register R) {
    switch (R) {
#define NEVERD_X64_REGISTER(Name, Decoder, Backend)                            \
  case X64Register::Name:                                                      \
    return Backend;
#include "neverd/emulation/X64Registers.def"
#undef NEVERD_X64_REGISTER
    default:
      return UC_X86_REG_INVALID;
    }
  }
};
class UnicornAArch64Machine final : public AArch64Machine {
public:
  UnicornStepper CPU;
  explicit UnicornAArch64Machine(PhysicalMemory &Memory) : CPU(Memory) {}
  llvm::Error step(AArch64MachineState &State,
                   std::chrono::steady_clock::time_point) override {
    if (auto E = CPU.synchronize())
      return E;
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  NEVERD_REGISTER_##Arch(Name, Backend)
#define NEVERD_REGISTER_X64(Name, Backend)
#define NEVERD_REGISTER_AArch64(Name, Backend)                                 \
  if (auto E = check(uc_reg_write(CPU.Engine, Backend,                         \
                                  &State.reg(AArch64Register::Name))))         \
    return E;
#include "neverd/emulation/Registers.def"
#undef NEVERD_REGISTER_AArch64
#define NEVERD_REGISTER_AArch64(Name, Backend)                                 \
  if (auto E = check(uc_reg_read(CPU.Engine, Backend,                          \
                                 &State.reg(AArch64Register::Name))))          \
    return E;
    if (auto E = CPU.run(State.reg(AArch64Register::PC)))
      return E;
#include "neverd/emulation/Registers.def"
#undef NEVERD_SCALAR_REGISTER
#undef NEVERD_REGISTER_X64
#undef NEVERD_REGISTER_AArch64
    // SIMD is outside this checked contract, so the typed vector cache is
    // preserved. The unrestricted software contract uses UnicornBackend.
    return llvm::Error::success();
  }
};
} // namespace
llvm::Expected<std::unique_ptr<X64Machine>>
createUnicornX64Machine(PhysicalMemory &Memory) {
  auto M = std::make_unique<UnicornX64Machine>(Memory);
  if (auto E = M->CPU.initialize(UC_ARCH_X86, UC_MODE_64))
    return E;
  return std::unique_ptr<X64Machine>(std::move(M));
}
llvm::Expected<std::unique_ptr<AArch64Machine>>
createUnicornAArch64Machine(PhysicalMemory &Memory) {
  auto M = std::make_unique<UnicornAArch64Machine>(Memory);
  if (auto E = M->CPU.initialize(UC_ARCH_ARM64, UC_MODE_ARM))
    return E;
  return std::unique_ptr<AArch64Machine>(std::move(M));
}
} // namespace neverd::emulation
