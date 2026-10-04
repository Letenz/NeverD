//===- HvfAArch64Machine.cpp - Native ARM64 instruction transport ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/aarch64/AArch64State.h"
#include "../../core/ExecutionDiagnostics.h"
#include "../MachineFactories.h"

#if defined(__APPLE__) && defined(__arm64__) && defined(NEVERD_EMULATION_HVF)
#include "HvfExecutor.h"

#include "llvm/Support/FormatVariadic.h"

#include <cstring>

namespace neverd::emulation {
namespace {
class ArmState {
public:
  explicit ArmState(hvf::Cpu CPU) : CPU(CPU) {}
  llvm::Error set(hv_reg_t R, uint64_t Value) {
    auto Status = hv_vcpu_set_reg(CPU, R, Value);
    return Status ? hvf::error("hv_vcpu_set_reg", Status)
                  : llvm::Error::success();
  }
  llvm::Error set(hv_sys_reg_t R, uint64_t Value) {
    auto Status = hv_vcpu_set_sys_reg(CPU, R, Value);
    return Status ? hvf::error("hv_vcpu_set_sys_reg", Status)
                  : llvm::Error::success();
  }
  llvm::Expected<uint64_t> get(hv_reg_t R) {
    uint64_t Value = 0;
    if (auto Status = hv_vcpu_get_reg(CPU, R, &Value))
      return hvf::error("hv_vcpu_get_reg", Status);
    return Value;
  }
  llvm::Expected<uint64_t> get(hv_sys_reg_t R) {
    uint64_t Value = 0;
    if (auto Status = hv_vcpu_get_sys_reg(CPU, R, &Value))
      return hvf::error("hv_vcpu_get_sys_reg", Status);
    return Value;
  }
  llvm::Expected<uint64_t> scalar(AArch64Register R, bool User) {
    if (unsigned(R) < aarch64::GPRCount)
      return get(hv_reg_t(HV_REG_X0 + unsigned(R)));
    switch (R) {
    case AArch64Register::PC:
      return get(HV_REG_PC);
    case AArch64Register::SP:
      return get(User ? HV_SYS_REG_SP_EL0 : HV_SYS_REG_SP_EL1);
    case AArch64Register::NZCV:
      return get(HV_REG_CPSR);
    case AArch64Register::TPIDR_EL0:
      return get(HV_SYS_REG_TPIDR_EL0);
    case AArch64Register::TPIDRRO_EL0:
      return get(HV_SYS_REG_TPIDRRO_EL0);
    case AArch64Register::TPIDR_EL1:
      return get(HV_SYS_REG_TPIDR_EL1);
    case AArch64Register::FPCR:
      return get(HV_REG_FPCR);
    case AArch64Register::FPSR:
      return get(HV_REG_FPSR);
    default:
      return diagnostic::error(diagnostic::Register);
    }
  }
  llvm::Error prepare(const AArch64MachineState &State) {
    using namespace aarch64;
    // Debug exits are routed to EL2, where PSTATE.D cannot mask them. Disable
    // software stepping for the immutable maintenance stub, then restore it
    // only after its distinct HVC exit has been authenticated.
    const std::pair<hv_sys_reg_t, uint64_t> System[] = {
        {HV_SYS_REG_TTBR0_EL1, LowRoot},
        {HV_SYS_REG_TTBR1_EL1, HighRoot},
        {HV_SYS_REG_TCR_EL1, TCR},
        {HV_SYS_REG_MAIR_EL1, MAIR},
        {HV_SYS_REG_SCTLR_EL1, SCTLR},
        {HV_SYS_REG_VBAR_EL1, VectorGPA},
        {HV_SYS_REG_CPACR_EL1, CPACR},
        {HV_SYS_REG_MDSCR_EL1, 0},
        {HV_SYS_REG_SP_EL0,
         State.UserMode ? State.reg(AArch64Register::SP) : 0},
        {HV_SYS_REG_SP_EL1,
         State.UserMode ? 0 : State.reg(AArch64Register::SP)},
        {HV_SYS_REG_TPIDR_EL0, State.reg(AArch64Register::TPIDR_EL0)},
        {HV_SYS_REG_TPIDRRO_EL0, State.reg(AArch64Register::TPIDRRO_EL0)},
        {HV_SYS_REG_TPIDR_EL1, State.reg(AArch64Register::TPIDR_EL1)},
        {HV_SYS_REG_ESR_EL1, 0},
        {HV_SYS_REG_FAR_EL1, 0},
        {HV_SYS_REG_CNTV_CTL_EL0, 0}};
    for (auto [R, Value] : System)
      if (auto E = set(R, Value))
        return E;
    for (unsigned N = 0; N < GPRCount; ++N)
      if (auto E = set(hv_reg_t(HV_REG_X0 + N), State.Registers[N]))
        return E;
    if (auto E = set(HV_REG_FPCR, State.reg(AArch64Register::FPCR)))
      return E;
    if (auto E = set(HV_REG_FPSR, State.reg(AArch64Register::FPSR)))
      return E;
    for (unsigned N = 0; N < VectorCount; ++N) {
      hv_simd_fp_uchar16_t Value;
      static_assert(sizeof(Value) == sizeof(State.Vectors[N]));
      std::memcpy(&Value, State.Vectors[N].data(), sizeof(Value));
      if (auto Status = hv_vcpu_set_simd_fp_reg(
              CPU, hv_simd_fp_reg_t(HV_SIMD_FP_REG_Q0 + N), Value))
        return hvf::error("hv_vcpu_set_simd_fp_reg", Status);
    }
    if (auto Status = hv_vcpu_set_trap_debug_exceptions(CPU, true))
      return hvf::unavailable("hv_vcpu_set_trap_debug_exceptions", Status);
    if (auto E = set(HV_REG_CPSR, PStateEL1h | PStateDAIF))
      return E;
    return set(HV_REG_PC, MaintenanceEntryGPA);
  }
  llvm::Error capture(AArch64MachineState &State) {
    return captureAArch64State(
        State, [&](AArch64Register R) { return scalar(R, State.UserMode); },
        [&](unsigned N) -> llvm::Expected<RegisterValue> {
          hv_simd_fp_uchar16_t Value;
          if (auto Status = hv_vcpu_get_simd_fp_reg(
                  CPU, hv_simd_fp_reg_t(HV_SIMD_FP_REG_Q0 + N), &Value))
            return hvf::error("hv_vcpu_get_simd_fp_reg", Status);
          RegisterValue Result{};
          std::memcpy(Result.data(), &Value, sizeof(Value));
          return Result;
        });
  }

private:
  hvf::Cpu CPU;
};
class HvfAArch64Machine final : public AArch64Machine {
public:
  HvfAArch64Machine(std::shared_ptr<hvf::Executor> Host,
                    MemoryProjection &Memory)
      : Host(std::move(Host), Memory) {}
  llvm::Error step(AArch64MachineState &State,
                   MachineRunControl Control) override {
    auto Next = State;
    auto E = Host.execute(Control, [&](hvf::Executor &Executor) -> llvm::Error {
      Control = Control.forNativeStep();
      ArmState Native(Executor.cpu());
      if (auto E = Native.prepare(State))
        return E;
      auto Enter = [&](bool Capture = false) -> llvm::Error {
        return Executor.run(Control, [&](bool Cancelled) -> llvm::Error {
          const auto &Exit = Executor.exit();
          if (Exit.reason == HV_EXIT_REASON_CANCELED && Cancelled)
            return llvm::Error::success();
          if (Exit.reason == HV_EXIT_REASON_EXCEPTION &&
              (Capture
                   ? ((Exit.exception.syndrome >>
                       aarch64::ExceptionClassShift) &
                      aarch64::ExceptionClassMask) == aarch64::StepFromLowerEL
                   : Exit.exception.syndrome == aarch64::MaintenanceSyndrome)) {
            // A guest synchronous fault may enter EL1 before the debug exit
            // reaches EL2. That exit is not proof the instruction completed.
            // ESR was cleared before entry; admitted instructions cannot
            // write it or change EL. Never publish an exception-vector PC.
            auto Syndrome = Native.get(HV_SYS_REG_ESR_EL1);
            if (!Syndrome)
              return Syndrome.takeError();
            auto PState = Native.get(HV_REG_CPSR);
            if (!PState)
              return PState.takeError();
            const auto Mode = Capture && State.UserMode ? aarch64::PStateEL0t
                                                        : aarch64::PStateEL1h;
            if (*Syndrome || (*PState & 0xf) != Mode)
              return diagnostic::error(diagnostic::ArmState);
            if (!Capture) {
              auto PC = Native.get(HV_REG_PC);
              if (!PC)
                return PC.takeError();
              // HVF reports the return PC after HVC. A vector gateway, an
              // early HVC or an unexpected branch cannot complete maintenance.
              if (*PC !=
                      aarch64::MaintenanceExitGPA + aarch64::InstructionBytes ||
                  *PState != (aarch64::PStateEL1h | aarch64::PStateDAIF))
                return diagnostic::error(diagnostic::ArmState);
            }
            return Capture ? Native.capture(Next) : llvm::Error::success();
          }
          return llvm::createStringError(
              llvm::inconvertibleErrorCode(),
              llvm::formatv("HVF ARM64 unexpected exit {0}, syndrome {1:x}, VA "
                            "{2:x}, IPA {3:x}",
                            uint32_t(Exit.reason), Exit.exception.syndrome,
                            Exit.exception.virtual_address,
                            Exit.exception.physical_address)
                  .str());
        });
      };
      if (auto E = Enter())
        return E;
      if (auto E =
              Native.set(HV_SYS_REG_MDSCR_EL1, aarch64::MDSCRSingleStep |
                                                   aarch64::MDSCRKernelDebug |
                                                   aarch64::MDSCRMonitorDebug))
        return E;
      if (auto E = Native.set(HV_REG_PC, State.reg(AArch64Register::PC)))
        return E;
      if (auto E = Native.set(
              HV_REG_CPSR,
              State.reg(AArch64Register::NZCV) |
                  (State.UserMode ? aarch64::PStateEL0t : aarch64::PStateEL1h) |
                  aarch64::PStateDAIF | aarch64::PStateSingleStep))
        return E;
      if (auto E = Enter(true))
        return E;
      if (Control.interrupted())
        return diagnostic::interrupted("HVF ARM64 capture interrupted",
                                       Control);
      return llvm::Error::success();
    });
    if (E)
      return E;
    State = Next;
    return llvm::Error::success();
  }

private:
  hvf::Binding Host;
};
} // namespace
llvm::Expected<std::unique_ptr<AArch64Machine>>
createHvfAArch64Machine(MemoryProjection &Memory) {
  auto Host = hvf::Executor::acquire();
  if (!Host)
    return Host.takeError();
  auto Machine = std::make_unique<HvfAArch64Machine>(std::move(*Host), Memory);
  if (auto E = verifyAArch64Machine(*Machine, Memory))
    return E;
  return std::unique_ptr<AArch64Machine>(std::move(Machine));
}
} // namespace neverd::emulation
#else
namespace neverd::emulation {
llvm::Expected<std::unique_ptr<AArch64Machine>>
createHvfAArch64Machine(MemoryProjection &) {
  return diagnostic::unavailable(diagnostic::Unavailable);
}
} // namespace neverd::emulation
#endif
