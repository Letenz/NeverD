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
#include "UnicornMemory.h"

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
  MemoryProjection &Memory;
  const bool UserMode;
  explicit UnicornStepper(MemoryProjection &Memory, bool UserMode)
      : Memory(Memory), UserMode(UserMode) {}
  ~UnicornStepper() {
    if (Engine)
      uc_close(Engine);
  }
  llvm::Error initialize(uc_arch Arch, uc_mode Mode) {
    if (auto E = check(uc_open(Arch, Mode, &Engine)))
      return E;
    if (auto E = check(
            uc_ctl_tlb_mode(Engine, UserMode ? UC_TLB_CPU : UC_TLB_VIRTUAL)))
      return E;
    if (UserMode)
      for (const auto &Range : Memory.registrations())
        if (auto E = check(uc_mem_map_ptr(Engine, Range.Physical, Range.Size,
                                          UC_PROT_ALL, Range.Backing)))
          return E;
    return initializeUnicornArchitecture(
        Engine, Arch == UC_ARCH_X86 ? GuestArchitecture::X64
                                    : GuestArchitecture::AArch64);
  }
  llvm::Error synchronize() {
    if (UserMode)
      return llvm::Error::success();
    if (MappedSpace.lock() == Memory.addressSpace() &&
        Generation == Memory.mappingGeneration())
      return llvm::Error::success();
    if (auto E = forEachUnicornRAMRange(
            Mapped, [&](uint64_t Address, uint64_t Size, const auto &) {
              return check(uc_mem_unmap(Engine, Address, Size));
            }))
      return E;
    Mapped = Memory.mappings();
    if (auto E = forEachUnicornRAMRange(
            Mapped, [&](uint64_t Address, uint64_t Size, const auto &Page) {
              return check(
                  uc_mem_map_ptr(Engine, Address, Size,
                                 Page.Permissions & GuestAccessPermissions,
                                 Memory.physicalPointer(Page.Physical)));
            }))
      return E;
    Generation = Memory.mappingGeneration();
    MappedSpace = Memory.addressSpace();
    return llvm::Error::success();
  }
  llvm::Error run(uint64_t PC, size_t Count = 1) {
    // Host backing writes bypass Unicorn's code-write invalidation. Discard
    // translated blocks before entering so aliases and self-modifying code
    // observe current authoritative bytes.
    if (auto E = check(uc_ctl_flush_tb(Engine)))
      return E;
    return check(uc_emu_start(Engine, PC, 0, 0, Count));
  }

private:
  uint64_t Generation = 0;
  std::weak_ptr<AddressSpace> MappedSpace;
  std::map<uint64_t, MemoryProjection::Page> Mapped;
};
class UnicornX64Machine final : public X64Machine {
public:
  UnicornStepper CPU;
  explicit UnicornX64Machine(MemoryProjection &Memory, bool UserMode)
      : CPU(Memory, UserMode) {}
  llvm::Error initialize() {
    if (auto E = CPU.initialize(UC_ARCH_X86, UC_MODE_64))
      return E;
    if (!CPU.UserMode)
      return llvm::Error::success();
    // In 64-bit Unicorn, writing CS/SS changes selectors only. SYSRET installs
    // real CPL3 segment caches before enabling translation. This private boot
    // instruction executes once, before any caller instruction or page table.
    // No monitor virtual address or guest allocation is needed after reset.
    uint64_t CR0 = x64::CR0 & ~x64::Paging, CR4 = x64::CR4;
    uc_x86_msr EFER{x64::EFERAddress, x64::EFER | x64::SystemCallEnable};
    uc_x86_msr STAR{x64::STARAddress, x64::UserSTAR};
    if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_CR0, &CR0)))
      return E;
    if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_CR4, &CR4)))
      return E;
    if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_MSR, &EFER)))
      return E;
    if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_MSR, &STAR)))
      return E;
    const uint8_t Bootstrap[] = {
#define NEVERD_UNICORN_USER_BOOTSTRAP(...) __VA_ARGS__
#include "UnicornArchitecture.def"
#undef NEVERD_UNICORN_USER_BOOTSTRAP
    };
    if (auto E = check(uc_mem_write(CPU.Engine, x64::BootstrapPC, Bootstrap,
                                    sizeof(Bootstrap))))
      return E;
    uint64_t PC = x64::BootstrapReturnPC, Flags = x64::InitialFlags;
    if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_RCX, &PC)))
      return E;
    if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_R11, &Flags)))
      return E;
    return CPU.run(x64::BootstrapPC);
  }
  llvm::Error step(X64MachineState &State, uint64_t Root,
                   MachineRunControl) override {
    if (auto E = CPU.synchronize())
      return E;
    if (CPU.UserMode) {
      uint64_t CR0 = x64::CR0;
      if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_CR3, &Root)))
        return E;
      if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_CR0, &CR0)))
        return E;
    }
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
  explicit UnicornAArch64Machine(MemoryProjection &Memory, bool UserMode)
      : CPU(Memory, UserMode) {}
  llvm::Error step(AArch64MachineState &State, MachineRunControl) override {
    if (auto E = CPU.synchronize())
      return E;
    if (CPU.UserMode) {
      // The pinned engine's direct PSTATE write does not rebuild cached EL
      // flags. Configure EL1 before CP_REG writes, then use architectural ERET
      // to enter EL0. Selector/mode metadata alone is not privilege evidence.
      uint64_t Mode = aarch64::PStateEL1h | aarch64::PStateDAIF;
      if (auto E = check(uc_reg_write(CPU.Engine, UC_ARM64_REG_PSTATE, &Mode)))
        return E;
#define NEVERD_AARCH64_SYSTEM_REGISTER(Name, Op0, Op1, CRn, CRm, Op2)          \
  auto Name = [&](uint64_t Value) {                                            \
    uc_arm64_cp_reg R{CRn, CRm, Op0, Op1, Op2, Value};                         \
    return check(uc_reg_write(CPU.Engine, UC_ARM64_REG_CP_REG, &R));           \
  };
#include "../../arch/aarch64/AArch64SystemRegisters.def"
#undef NEVERD_AARCH64_SYSTEM_REGISTER
      if (auto E = Ttbr0El1(aarch64::LowRoot))
        return E;
      if (auto E = Ttbr1El1(aarch64::HighRoot))
        return E;
      if (auto E = TcrEl1(aarch64::TCR))
        return E;
      if (auto E = MairEl1(aarch64::MAIR))
        return E;
      if (auto E = SctlrEl1(aarch64::SCTLR))
        return E;
      if (auto E = VbarEl1(aarch64::VectorGPA))
        return E;
      if (auto E = ElrEl1(State.reg(AArch64Register::PC)))
        return E;
      if (auto E = SpsrEl1(aarch64::PStateEL0t | aarch64::PStateDAIF |
                           State.reg(AArch64Register::NZCV)))
        return E;
      if (auto E =
              CPU.run(aarch64::EntryGPA, std::size(aarch64::Maintenance) +
                                             aarch64::GateReturnInstructions))
        return E;
    }
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
createUnicornX64Machine(MemoryProjection &Memory, bool UserMode) {
  auto M = std::make_unique<UnicornX64Machine>(Memory, UserMode);
  if (auto E = M->initialize())
    return E;
  return std::unique_ptr<X64Machine>(std::move(M));
}
llvm::Expected<std::unique_ptr<AArch64Machine>>
createUnicornAArch64Machine(MemoryProjection &Memory, bool UserMode) {
  auto M = std::make_unique<UnicornAArch64Machine>(Memory, UserMode);
  if (auto E = M->CPU.initialize(UC_ARCH_ARM64, UC_MODE_ARM))
    return E;
  return std::unique_ptr<AArch64Machine>(std::move(M));
}
} // namespace neverd::emulation
