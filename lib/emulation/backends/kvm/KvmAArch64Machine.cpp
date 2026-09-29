//===- KvmAArch64Machine.cpp - Linux ARM64 checked execution --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/aarch64/AArch64Machine.h"
#include "../../core/ExecutionDiagnostics.h"
#include "../MachineFactories.h"
#if defined(__linux__) && defined(__aarch64__) && defined(NEVERD_EMULATION_KVM)
#include "KvmVM.h"

#include "llvm/Support/FormatVariadic.h"

#include <cerrno>
#include <cstddef>

namespace neverd::emulation {
namespace {
constexpr uint64_t coreRegister(size_t Offset) {
  return KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
         (Offset / sizeof(uint32_t));
}
#define NEVERD_AARCH64_SYSTEM_REGISTER(Name, Op0, Op1, CRn, CRm, Op2)          \
  constexpr uint64_t Name =                                                    \
      KVM_REG_ARM64 | ARM64_SYS_REG(Op0, Op1, CRn, CRm, Op2);
#include "../../arch/aarch64/AArch64SystemRegisters.def"
#undef NEVERD_AARCH64_SYSTEM_REGISTER
class KvmAArch64Machine final : public AArch64Machine, public KvmVM {
public:
  llvm::Error set(uint64_t Name, uint64_t Value) {
    kvm_one_reg R{Name, reinterpret_cast<uintptr_t>(&Value)};
    if (ioctl(CPU, KVM_SET_ONE_REG, &R) < 0)
      return diagnostic::error(diagnostic::KvmState);
    return llvm::Error::success();
  }
  llvm::Error get(uint64_t Name, uint64_t &Value) {
    kvm_one_reg R{Name, reinterpret_cast<uintptr_t>(&Value)};
    if (ioctl(CPU, KVM_GET_ONE_REG, &R) < 0)
      return diagnostic::error(diagnostic::KvmState);
    return llvm::Error::success();
  }
  llvm::Error enter(std::chrono::steady_clock::time_point Deadline) {
    kvm_guest_debug Debug{};
    Debug.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP;
    if (ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debug) < 0)
      return diagnostic::unavailable(diagnostic::KvmCapabilities);
    if (auto E = runUntilExit(
            Deadline + std::chrono::microseconds(
                           execution_limits::NativeStepGraceMicroseconds)))
      return E;
    if (Run->exit_reason != KVM_EXIT_DEBUG ||
        ((Run->debug.arch.hsr >> aarch64::ExceptionClassShift) &
         aarch64::ExceptionClassMask) != aarch64::StepFromLowerEL)
      return diagnostic::error(diagnostic::ArmState);
    return llvm::Error::success();
  }
  llvm::Error step(AArch64MachineState &State,
                   std::chrono::steady_clock::time_point Deadline) override {
    using namespace aarch64;
    const auto PC = coreRegister(offsetof(kvm_regs, regs.pc));
    const auto PState = coreRegister(offsetof(kvm_regs, regs.pstate));
    // Rebuilds change page-table bytes, not the translation cache. Execute the
    // immutable maintenance gate under host single-step before every entry.
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
    if (auto E = set(PState, PStateEL1h | PStateDAIF))
      return E;
    if (auto E = set(PC, EntryGPA))
      return E;
    for (unsigned N = 0; N < std::size(Maintenance); ++N) {
      if (auto E = enter(Deadline))
        return E;
      uint64_t ActualPC = 0;
      if (auto E = get(PC, ActualPC))
        return E;
      if (ActualPC != EntryGPA + (N + 1) * InstructionBytes)
        return diagnostic::error(diagnostic::ArmState);
    }
    for (unsigned N = 0; N < GPRCount; ++N)
      if (auto E =
              set(coreRegister(offsetof(kvm_regs, regs.regs) + N * WordBytes),
                  State.Registers[N]))
        return E;
    if (auto E = set(coreRegister(offsetof(kvm_regs, sp_el1)),
                     State.reg(AArch64Register::SP)))
      return E;
    if (auto E = set(PState, PStateEL1h | PStateDAIF |
                                 State.reg(AArch64Register::NZCV)))
      return E;
    if (auto E = set(PC, State.reg(AArch64Register::PC)))
      return E;
    if (auto E = enter(Deadline))
      return E;
    for (unsigned N = 0; N < GPRCount; ++N)
      if (auto E =
              get(coreRegister(offsetof(kvm_regs, regs.regs) + N * WordBytes),
                  State.Registers[N]))
        return E;
    if (auto E = get(coreRegister(offsetof(kvm_regs, sp_el1)),
                     State.reg(AArch64Register::SP)))
      return E;
    if (auto E = get(PC, State.reg(AArch64Register::PC)))
      return E;
    uint64_t Flags = 0;
    if (auto E = get(PState, Flags))
      return E;
    State.reg(AArch64Register::NZCV) = Flags & NZCVMask;
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
    return diagnostic::unavailable(diagnostic::KvmCapabilities);
  kvm_guest_debug Debug{};
  Debug.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP;
  if (ioctl(M->CPU, KVM_SET_GUEST_DEBUG, &Debug) < 0)
    return diagnostic::unavailable(diagnostic::KvmCapabilities);
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
