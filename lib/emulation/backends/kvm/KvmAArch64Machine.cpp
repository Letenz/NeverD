//===- KvmAArch64Machine.cpp - Linux ARM64 checked execution --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/aarch64/AArch64State.h"
#include "../../core/ExecutionDiagnostics.h"
#include "../MachineFactories.h"
#if defined(__linux__) && defined(__aarch64__) && defined(NEVERD_EMULATION_KVM)
#include "KvmVM.h"

#include "llvm/Support/ErrorHandling.h"

#include <array>
#include <cerrno>
#include <cstddef>

namespace neverd::emulation {
namespace {
constexpr uint64_t coreRegister(size_t Offset,
                                uint64_t Size = KVM_REG_SIZE_U64) {
  return KVM_REG_ARM64 | Size | KVM_REG_ARM_CORE | (Offset / sizeof(uint32_t));
}
#define NEVERD_AARCH64_SYSTEM_REGISTER(Name, Op0, Op1, CRn, CRm, Op2)          \
  constexpr uint64_t Name =                                                    \
      KVM_REG_ARM64 | ARM64_SYS_REG(Op0, Op1, CRn, CRm, Op2);
#include "../../arch/aarch64/AArch64SystemRegisters.def"
#undef NEVERD_AARCH64_SYSTEM_REGISTER
uint64_t scalarRegister(AArch64Register Register, bool UserMode) {
  if (unsigned(Register) < aarch64::GPRCount)
    return coreRegister(offsetof(kvm_regs, regs.regs) +
                        unsigned(Register) * aarch64::WordBytes);
  switch (Register) {
  case AArch64Register::SP:
    return coreRegister(UserMode ? offsetof(kvm_regs, regs.sp)
                                 : offsetof(kvm_regs, sp_el1));
  case AArch64Register::PC:
    return coreRegister(offsetof(kvm_regs, regs.pc));
  case AArch64Register::NZCV:
    return coreRegister(offsetof(kvm_regs, regs.pstate));
  case AArch64Register::TPIDR_EL0:
    return TpidrEl0;
  case AArch64Register::TPIDRRO_EL0:
    return TpidrroEl0;
  case AArch64Register::TPIDR_EL1:
    return TpidrEl1;
  case AArch64Register::FPCR:
    return coreRegister(offsetof(kvm_regs, fp_regs.fpcr), KVM_REG_SIZE_U32);
  case AArch64Register::FPSR:
    return coreRegister(offsetof(kvm_regs, fp_regs.fpsr), KVM_REG_SIZE_U32);
  default:
    llvm_unreachable(diagnostic::Register);
  }
}
uint64_t vectorRegister(unsigned Index) {
  return coreRegister(offsetof(kvm_regs, fp_regs.vregs) +
                          Index * sizeof(RegisterValue),
                      KVM_REG_SIZE_U128);
}
class KvmAArch64Machine final : public AArch64Machine, public KvmVM {
public:
  template <typename T> llvm::Error set(uint64_t Name, const T &Value) {
    kvm_one_reg R{Name, reinterpret_cast<uintptr_t>(&Value)};
    if (ioctl(CPU, KVM_SET_ONE_REG, &R) < 0)
      return diagnostic::error(diagnostic::KvmState);
    return llvm::Error::success();
  }
  template <typename T> llvm::Error get(uint64_t Name, T &Value) {
    kvm_one_reg R{Name, reinterpret_cast<uintptr_t>(&Value)};
    if (ioctl(CPU, KVM_GET_ONE_REG, &R) < 0)
      return diagnostic::error(diagnostic::KvmState);
    return llvm::Error::success();
  }
  llvm::Error enter(MachineRunControl Control,
                    KvmRunControl::StateTransfer Prepare = {},
                    KvmRunControl::StateTransfer Capture = {}) {
    auto Before = [&]() -> llvm::Error {
      if (Prepare)
        if (auto E = Prepare())
          return E;
      kvm_guest_debug Debug{};
      Debug.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP;
      if (ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debug) < 0)
        return diagnostic::unavailable(diagnostic::KvmCapabilities,
                                       BackendAvailability::MissingCapability);
      return llvm::Error::success();
    };
    auto After = [&]() -> llvm::Error {
      if (Run->exit_reason != KVM_EXIT_DEBUG ||
          ((Run->debug.arch.hsr >> aarch64::ExceptionClassShift) &
           aarch64::ExceptionClassMask) != aarch64::StepFromLowerEL)
        return diagnostic::error(diagnostic::ArmState);
      return Capture ? Capture() : llvm::Error::success();
    };
    return runUntilExit(Control, Before, After);
  }
  llvm::Error step(AArch64MachineState &State,
                   MachineRunControl Control) override {
    using namespace aarch64;
    // Maintenance and the admitted instruction share one allowance; retries
    // or additional maintenance entries must not extend it.
    Control = Control.forNativeStep();
    const auto PC = coreRegister(offsetof(kvm_regs, regs.pc));
    const auto PState = coreRegister(offsetof(kvm_regs, regs.pstate));
    const uint64_t Mode = State.UserMode ? PStateEL0t : PStateEL1h;
    // Rebuilds change page-table bytes, not the translation cache. Execute the
    // immutable maintenance gate under host single-step before every entry.
    auto PrepareGate = [&]() -> llvm::Error {
      if (auto E = set(Ttbr0El1, LowRoot))
        return E;
      if (auto E = set(Ttbr1El1, HighRoot))
        return E;
      if (auto E = set(TcrEl1, TCR))
        return E;
      if (auto E = set(MairEl1, MAIR))
        return E;
      if (auto E = set(SctlrEl1, SCTLR))
        return E;
      if (auto E = set(VbarEl1, VectorGPA))
        return E;
      if (auto E = set(CpacrEl1, CPACR))
        return E;
      if (auto E = set(PState, PStateEL1h | PStateDAIF))
        return E;
      return set(PC, EntryGPA);
    };
    for (unsigned N = 0; N < std::size(Maintenance); ++N) {
      uint64_t ActualPC = 0;
      const KvmRunControl::StateTransfer Prepare =
          N ? KvmRunControl::StateTransfer{} : PrepareGate;
      if (auto E = enter(Control, Prepare, [&] { return get(PC, ActualPC); }))
        return E;
      if (ActualPC != EntryGPA + (N + 1) * InstructionBytes)
        return diagnostic::error(diagnostic::ArmState);
    }
    auto PrepareGuest = [&]() -> llvm::Error {
      for (unsigned Index = 0; Index < State.Registers.size(); ++Index) {
        const auto Register = AArch64Register(Index);
        const uint64_t Value = Register == AArch64Register::NZCV
                                   ? Mode | PStateDAIF | State.reg(Register)
                                   : State.reg(Register);
        const auto Name = scalarRegister(Register, State.UserMode);
        auto E = Register == AArch64Register::FPCR ||
                         Register == AArch64Register::FPSR
                     ? set(Name, uint32_t(Value))
                     : set(Name, Value);
        if (E)
          return E;
      }
      for (unsigned Index = 0; Index < State.Vectors.size(); ++Index)
        if (auto E = set(vectorRegister(Index), State.Vectors[Index]))
          return E;
      return llvm::Error::success();
    };
    // This packet is private to the entry worker until its complete capture
    // has been acknowledged. Scalar and vector reads share one ISA commit.
    auto Captured = State;
    auto CaptureGuest = [&]() -> llvm::Error {
      return captureAArch64State(
          Captured,
          [&](AArch64Register Register) -> llvm::Expected<uint64_t> {
            const auto Name = scalarRegister(Register, State.UserMode);
            if (Register == AArch64Register::FPCR ||
                Register == AArch64Register::FPSR) {
              uint32_t Value = 0;
              if (auto E = get(Name, Value))
                return E;
              return Value;
            }
            uint64_t Value = 0;
            if (auto E = get(Name, Value))
              return E;
            return Value;
          },
          [&](unsigned Index) -> llvm::Expected<RegisterValue> {
            RegisterValue Value{};
            if (auto E = get(vectorRegister(Index), Value))
              return E;
            return Value;
          });
    };
    if (auto E = enter(Control, PrepareGuest, CaptureGuest))
      return E;
    State = Captured;
    return llvm::Error::success();
  }
};
} // namespace
llvm::Expected<std::unique_ptr<AArch64Machine>>
createKvmAArch64Machine(MemoryProjection &Memory) {
  auto M = std::make_unique<KvmAArch64Machine>();
  if (auto E = M->initialize(Memory.registrations()))
    return E;
  kvm_vcpu_init Init{};
  if (ioctl(M->VM, KVM_ARM_PREFERRED_TARGET, &Init) < 0 ||
      ioctl(M->CPU, KVM_ARM_VCPU_INIT, &Init) < 0)
    return diagnostic::unavailable(diagnostic::KvmCapabilities,
                                   BackendAvailability::MissingCapability);
  kvm_guest_debug Debug{};
  Debug.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP;
  if (ioctl(M->CPU, KVM_SET_GUEST_DEBUG, &Debug) < 0)
    return diagnostic::unavailable(diagnostic::KvmCapabilities,
                                   BackendAvailability::MissingCapability);
  if (auto E = verifyAArch64Machine(*M, Memory))
    return E;
  return std::unique_ptr<AArch64Machine>(std::move(M));
}
} // namespace neverd::emulation
#else
namespace neverd::emulation {
llvm::Expected<std::unique_ptr<AArch64Machine>>
createKvmAArch64Machine(MemoryProjection &) {
  return diagnostic::unavailable(diagnostic::Unavailable);
}
} // namespace neverd::emulation
#endif
