//===- UnicornMachine.cpp - Portable checked-contract execution ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/aarch64/AArch64State.h"
#include "../../arch/x86_64/X64Exception.h"
#include "../../arch/x86_64/X64Machine.h"
#include "../../core/ExecutionDiagnostics.h"
#include "../MachineFactories.h"
#include "UnicornArchitecture.h"
#include "UnicornMemory.h"

#include "llvm/ADT/ScopeExit.h"

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
    if (auto E = initializeUnicornArchitecture(
            Engine, Arch == UC_ARCH_X86 ? GuestArchitecture::X64
                                        : GuestArchitecture::AArch64))
      return E;
    uc_hook Hook;
    return check(uc_hook_add(Engine, &Hook, UC_HOOK_CODE,
                             reinterpret_cast<void *>(entry), this, 1, 0));
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
  llvm::Error run(uint64_t PC, size_t Count = 1,
                  const MachineRunControl *Control = nullptr) {
    ActiveControl = Control;
    Cancelled = false;
    auto Release = llvm::scope_exit([&] { ActiveControl = nullptr; });
    if (interrupted()) {
      Cancelled = true;
      return diagnostic::interrupted(diagnostic::UnicornRun, *Control);
    }
    // Host backing writes bypass Unicorn's code-write invalidation. Discard
    // translated blocks before entering so aliases and self-modifying code
    // observe current authoritative bytes.
    if (auto E = check(uc_ctl_flush_tb(Engine)))
      return E;
    if (interrupted()) {
      Cancelled = true;
      return diagnostic::interrupted(diagnostic::UnicornRun, *Control);
    }
    const auto Status = uc_emu_start(Engine, PC, 0, 0, Count);
    if (Cancelled)
      return diagnostic::interrupted(diagnostic::UnicornRun, *Control);
    if (Status != UC_ERR_OK)
      return check(Status);
    if (interrupted())
      return diagnostic::interrupted(diagnostic::UnicornRun, *Control);
    return llvm::Error::success();
  }
  bool entryCancelled() const { return Cancelled; }

private:
  // The synchronous engine call owns this borrow. Its instruction hook closes
  // the race between the caller's final check and actual guest execution.
  const MachineRunControl *ActiveControl = nullptr;
  bool Cancelled = false;
  bool interrupted() const {
    return ActiveControl && ActiveControl->interrupted();
  }
  static void entry(uc_engine *Engine, uint64_t, uint32_t, void *UserData) {
    auto &CPU = *static_cast<UnicornStepper *>(UserData);
    if (CPU.interrupted()) {
      CPU.Cancelled = true;
      uc_emu_stop(Engine);
    }
  }
  uint64_t Generation = 0;
  std::weak_ptr<AddressSpace> MappedSpace;
  std::map<uint64_t, MemoryProjection::Page> Mapped;
};
class UnicornX64Machine final : public X64Machine {
public:
  uint32_t mxcsrMask() const override {
    return x64::fp::ArchitecturalMXCSRMask;
  }
  UnicornStepper CPU;
  explicit UnicornX64Machine(MemoryProjection &Memory, bool UserMode)
      : CPU(Memory, UserMode) {}
  llvm::Error initialize() {
    if (auto E = CPU.initialize(UC_ARCH_X86, UC_MODE_64))
      return E;
    uc_hook Hook;
    if (auto E =
            check(uc_hook_add(CPU.Engine, &Hook, UC_HOOK_INTR,
                              reinterpret_cast<void *>(exception), this, 1, 0)))
      return E;
    if (auto E =
            check(uc_hook_add(CPU.Engine, &Hook, UC_HOOK_INSN_INVALID,
                              reinterpret_cast<void *>(invalid), this, 1, 0)))
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
    if (auto E = CPU.run(x64::BootstrapPC))
      return E;
    if (PendingException)
      return diagnostic::error(x64::exceptiontext::Bootstrap);
    uint64_t ReturnedPC = 0;
    if (auto E = check(uc_reg_read(CPU.Engine, UC_X86_REG_RIP, &ReturnedPC)))
      return E;
    if (ReturnedPC != PC)
      return diagnostic::error(x64::exceptiontext::Bootstrap);
    return llvm::Error::success();
  }
  llvm::Error step(X64MachineState &State, uint64_t Root,
                   MachineRunControl Control) override {
    Control = Control.forNativeStep();
    if (auto E = validateX64FPState(State.FP))
      return E;
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
    if (auto E =
            check(uc_reg_write(CPU.Engine, UC_X86_REG_FS_BASE, &State.FSBase)))
      return E;
    for (unsigned I = 0; I < State.Xmm.size(); ++I)
      if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_XMM0 + I,
                                      State.Xmm[I].data())))
        return E;
    if (auto E =
            check(uc_reg_write(CPU.Engine, UC_X86_REG_MXCSR, &State.MXCSR)))
      return E;
#define NEVERD_X64_FP_CONTROL(Name, Member, Type, UC, Offset)                  \
  if (auto E =                                                                 \
          check(uc_reg_write(CPU.Engine, UC_X86_REG_##UC, &State.FP.Member)))  \
    return E;
#include "../../arch/x86_64/X64FPState.def"
#undef NEVERD_X64_FP_CONTROL
    for (unsigned I = 0; I < State.FP.Registers.size(); ++I)
      if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_FP0 + I,
                                      State.FP.Registers[I].data())))
        return E;
    const uint16_t Tag = State.FP.fullTag();
    if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_FPTAG, &Tag)))
      return E;
    PendingException.reset();
    auto RunError = CPU.run(State.reg(X64Register::PC), 1, &Control);
    if (RunError && (!PendingException || CPU.entryCancelled()))
      return RunError;
    llvm::consumeError(std::move(RunError));
    auto Next = State;
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  if (auto E = check(uc_reg_read(CPU.Engine, registerID(X64Register::Name),    \
                                 &Next.reg(X64Register::Name))))               \
    return E;
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
    for (unsigned I = 0; I < Next.Xmm.size(); ++I)
      if (auto E = check(
              uc_reg_read(CPU.Engine, UC_X86_REG_XMM0 + I, Next.Xmm[I].data())))
        return E;
    if (auto E = check(uc_reg_read(CPU.Engine, UC_X86_REG_MXCSR, &Next.MXCSR)))
      return E;
#define NEVERD_X64_FP_CONTROL(Name, Member, Type, UC, Offset)                  \
  if (auto E =                                                                 \
          check(uc_reg_read(CPU.Engine, UC_X86_REG_##UC, &Next.FP.Member)))    \
    return E;
#include "../../arch/x86_64/X64FPState.def"
#undef NEVERD_X64_FP_CONTROL
    for (unsigned I = 0; I < Next.FP.Registers.size(); ++I)
      if (auto E = check(uc_reg_read(CPU.Engine, UC_X86_REG_FP0 + I,
                                     Next.FP.Registers[I].data())))
        return E;
    uint16_t NextTag = 0;
    if (auto E = check(uc_reg_read(CPU.Engine, UC_X86_REG_FPTAG, &NextTag)))
      return E;
    Next.FP.setFullTag(NextTag);
    if (PendingException) {
      std::optional<uint64_t> Address;
      if (*PendingException == unsigned(x64::ExceptionVector::PageFault)) {
        uint64_t CR2 = 0;
        if (auto E = check(uc_reg_read(CPU.Engine, UC_X86_REG_CR2, &CR2)))
          return E;
        Address = CR2;
      }
      State = Next;
      return llvm::make_error<X64ExceptionError>(
          X64Exception{*PendingException, std::nullopt, Address});
    }
    if (Control.interrupted())
      return diagnostic::interrupted(diagnostic::UnicornRun, Control);
    State = Next;
    return llvm::Error::success();
  }

private:
  std::optional<unsigned> PendingException;
  static void exception(uc_engine *Engine, uint32_t Vector, void *Opaque) {
    auto &Machine = *static_cast<UnicornX64Machine *>(Opaque);
    if (!Machine.PendingException)
      Machine.PendingException = Vector;
    uc_emu_stop(Engine);
  }
  static bool invalid(uc_engine *Engine, void *Opaque) {
    exception(Engine, unsigned(x64::ExceptionVector::InvalidOpcode), Opaque);
    return false;
  }
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
  llvm::Error step(AArch64MachineState &State,
                   MachineRunControl Control) override {
    Control = Control.forNativeStep();
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
      if (auto E = CPU.run(aarch64::EntryGPA,
                           std::size(aarch64::Maintenance) +
                               aarch64::GateReturnInstructions,
                           &Control))
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
#undef NEVERD_SCALAR_REGISTER
#undef NEVERD_REGISTER_X64
#undef NEVERD_REGISTER_AArch64
    for (unsigned Index = 0; Index < State.Vectors.size(); ++Index)
      if (auto E = check(uc_reg_write(CPU.Engine, UC_ARM64_REG_Q0 + Index,
                                      State.Vectors[Index].data())))
        return E;
    if (auto E = CPU.run(State.reg(AArch64Register::PC), 1, &Control))
      return E;
    auto Next = State;
    if (auto E = captureAArch64State(
            Next,
            [&](AArch64Register Register) -> llvm::Expected<uint64_t> {
              uint64_t Value = 0;
              if (auto E = check(
                      uc_reg_read(CPU.Engine, registerID(Register), &Value)))
                return E;
              return Value;
            },
            [&](unsigned Index) -> llvm::Expected<RegisterValue> {
              RegisterValue Value{};
              if (auto E = check(uc_reg_read(
                      CPU.Engine, UC_ARM64_REG_Q0 + Index, Value.data())))
                return E;
              return Value;
            }))
      return E;
    if (Control.interrupted())
      return diagnostic::interrupted(diagnostic::UnicornRun, Control);
    State = Next;
    return llvm::Error::success();
  }

private:
  static int registerID(AArch64Register Register) {
    switch (Register) {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  NEVERD_REGISTER_##Arch(Name, Backend)
#define NEVERD_REGISTER_X64(Name, Backend)
#define NEVERD_REGISTER_AArch64(Name, Backend)                                 \
  case AArch64Register::Name:                                                  \
    return Backend;
#include "neverd/emulation/Registers.def"
#undef NEVERD_REGISTER_AArch64
#undef NEVERD_REGISTER_X64
#undef NEVERD_SCALAR_REGISTER
    }
    return UC_ARM64_REG_INVALID;
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
