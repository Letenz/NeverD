//===- WhpAArch64Machine.cpp - Windows ARM64 checked execution ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/aarch64/AArch64State.h"
#include "../../core/ExecutionDiagnostics.h"
#include "../MachineFactories.h"
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__)) &&          \
    defined(NEVERD_EMULATION_WHP)
#include "WhpResourceCache.h"
#include "WhpVirtualProcessor.h"

#include "llvm/Support/ErrorHandling.h"

#include <vector>

namespace neverd::emulation {
namespace {
WHV_REGISTER_NAME scalarRegister(AArch64Register Register, bool UserMode) {
  if (unsigned(Register) < aarch64::GPRCount)
    return WHV_REGISTER_NAME(WHvArm64RegisterX0 + unsigned(Register));
  switch (Register) {
  case AArch64Register::SP:
    return UserMode ? WHvArm64RegisterSpEl0 : WHvArm64RegisterSpEl1;
  case AArch64Register::PC:
    return WHvArm64RegisterElrEl1;
  case AArch64Register::NZCV:
    return WHvArm64RegisterSpsrEl1;
  case AArch64Register::TPIDR_EL0:
    return WHvArm64RegisterTpidrEl0;
  case AArch64Register::TPIDRRO_EL0:
    return WHvArm64RegisterTpidrroEl0;
  case AArch64Register::TPIDR_EL1:
    return WHvArm64RegisterTpidrEl1;
  case AArch64Register::FPCR:
    return WHvArm64RegisterFpcr;
  case AArch64Register::FPSR:
    return WHvArm64RegisterFpsr;
  default:
    llvm_unreachable(diagnostic::Register);
  }
}
llvm::Expected<std::unique_ptr<WhpVirtualProcessor>>
createWhpArmProcessor(MemoryProjection &Memory);
class WhpAArch64Machine final : public AArch64Machine {
public:
  explicit WhpAArch64Machine(MemoryProjection &Memory)
      : Memory(Memory),
        Binding([&Memory] { return createWhpArmProcessor(Memory); },
                Memory.parallelEnabled()) {}
  llvm::Error initialize() { return Binding.initialize(); }
  llvm::Error step(AArch64MachineState &State,
                   MachineRunControl Control) override {
    Control = Control.forNativeStep();
    auto Active = Binding.acquire(Control);
    if (!Active)
      return Active.takeError();
    auto &Host = **Active;
    auto &API = Host.API;
    const auto Partition = Host.Partition;
    using namespace aarch64;
    std::vector<WHV_REGISTER_NAME> Names;
    std::vector<WHV_REGISTER_VALUE> Values;
    auto Add = [&](WHV_REGISTER_NAME Name, uint64_t Value) {
      const unsigned Index = Names.size();
      WHV_REGISTER_VALUE V{};
      V.Reg64 = Value;
      Names.push_back(Name);
      Values.push_back(V);
      return Index;
    };
    const uint64_t Mode = State.UserMode ? PStateEL0t : PStateEL1h;
    for (unsigned Index = 0; Index < State.Registers.size(); ++Index) {
      const auto Register = AArch64Register(Index);
      const auto Value = Register == AArch64Register::NZCV
                             ? State.reg(Register) | Mode |
                                   (PStateDAIF & ~PStateDebugMask) |
                                   PStateSingleStep
                             : State.reg(Register);
      Add(scalarRegister(Register, State.UserMode), Value);
    }
    const unsigned VectorBegin = Names.size();
    for (unsigned Index = 0; Index < State.Vectors.size(); ++Index) {
      WHV_REGISTER_VALUE Value{};
      Value.Reg128.Low64 = State.Vectors[Index][0];
      Value.Reg128.High64 = State.Vectors[Index][1];
      Names.push_back(WHV_REGISTER_NAME(WHvArm64RegisterQ0 + Index));
      Values.push_back(Value);
    }
    const unsigned StateCount = Names.size();
    Add(WHvArm64RegisterPc, EntryGPA);
    Add(WHvArm64RegisterPstate, PStateEL1h | PStateDAIF);
    Add(WHvArm64RegisterMdscrEl1,
        MDSCRSingleStep | MDSCRKernelDebug | MDSCRMonitorDebug);
    // A slot keeps the same ASID and physical window on reuse. Its immutable
    // entry VA therefore remains valid even before the first TLBI in the
    // maintenance gateway; that TLBI retires all former guest translations.
    const uint64_t ASID = uint64_t(Host.ProcessorIndex + 1) << ASIDShift;
    Add(WHvArm64RegisterTtbr0El1, Memory.transportPhysical(LowRoot) | ASID);
    Add(WHvArm64RegisterTtbr1El1, Memory.transportPhysical(HighRoot) | ASID);
    Add(WHvArm64RegisterTcrEl1, TCR);
    Add(WHvArm64RegisterMairEl1, MAIR);
    Add(WHvArm64RegisterSctlrEl1, SCTLR);
    Add(WHvArm64RegisterVbarEl1, VectorGPA);
    Add(WHvArm64RegisterCpacrEl1, CPACR);
    if (const auto Status = API.WHvSetVirtualProcessorRegisters(
            Partition, Host.ProcessorIndex, Names.data(), Names.size(),
            Values.data());
        FAILED(Status))
      return whpError(diagnostic::WhpState, Status,
                      whp::operation::WHvSetVirtualProcessorRegisters);
    // ARM64 WHP does not expose x64's exception-exit bitmap. The immutable EL1
    // vector gateway returns through an intercepted HVC after one debug step.
    WHV_RUN_VP_EXIT_CONTEXT Exit{};
    auto Next = State;
    auto Complete = [&]() -> llvm::Error {
      if (Exit.ExitReason != WHvRunVpExitReasonHypercall ||
          Exit.Hypercall.Header.Pc !=
              VectorGPA + (State.UserMode ? LowerELVector : CurrentELVector) ||
          Exit.Hypercall.Immediate)
        return diagnostic::error(diagnostic::WhpExit);
      Names.resize(StateCount);
      Values.resize(StateCount);
      const auto Syndrome = Add(WHvArm64RegisterEsrEl1, 0);
      if (const auto Status = API.WHvGetVirtualProcessorRegisters(
              Partition, Host.ProcessorIndex, Names.data(), Names.size(),
              Values.data());
          FAILED(Status))
        return whpError(diagnostic::WhpState, Status,
                        whp::operation::WHvGetVirtualProcessorRegisters);
      if (((Values[Syndrome].Reg64 >> ExceptionClassShift) &
           ExceptionClassMask) !=
          (State.UserMode ? StepFromLowerEL : StepFromEL1))
        return diagnostic::error(diagnostic::ArmState);
      return captureAArch64State(
          Next,
          [&](AArch64Register Register) -> llvm::Expected<uint64_t> {
            return Values[unsigned(Register)].Reg64;
          },
          [&](unsigned Index) -> llvm::Expected<RegisterValue> {
            const auto &Value = Values[VectorBegin + Index].Reg128;
            return RegisterValue{Value.Low64, Value.High64};
          });
    };
    if (auto E = Host.run(Exit, Control, Complete))
      return E;
    if (Control.interrupted())
      return diagnostic::interrupted(diagnostic::WhpRun, Control);
    State = Next;
    return llvm::Error::success();
  }

private:
  MemoryProjection &Memory;
  WhpResourceBinding<WhpVirtualProcessor> Binding;
};
llvm::Error configureWhpArmPartition(WhpAPI &API,
                                     WHV_PARTITION_HANDLE &Partition) {
  if (auto E = API.load())
    return E;
  WHV_CAPABILITY C{};
  if (const auto Status = API.WHvGetCapability(
          WHvCapabilityCodeHypervisorPresent, &C, sizeof(C), nullptr);
      FAILED(Status))
    return whpUnavailable(diagnostic::WhpArmCapability, Status,
                          whp::operation::WHvGetCapability);
  if (!C.HypervisorPresent)
    return diagnostic::unavailable(diagnostic::WhpArmCapability,
                                   BackendAvailability::MissingCapability);
  if (const auto Status = API.WHvGetCapability(WHvCapabilityCodeExtendedVmExits,
                                               &C, sizeof(C), nullptr);
      FAILED(Status))
    return whpUnavailable(diagnostic::WhpArmCapability, Status,
                          whp::operation::WHvGetCapability);
  if (!C.ExtendedVmExits.HypercallExit)
    return diagnostic::unavailable(diagnostic::WhpArmCapability,
                                   BackendAvailability::MissingCapability);
  if (const auto Status = API.WHvCreatePartition(&Partition); FAILED(Status))
    return whpError(diagnostic::WhpCreate, Status,
                    whp::operation::WHvCreatePartition);
  WHV_PARTITION_PROPERTY P{};
  P.ProcessorCount = ProcessorCount;
  if (const auto Status = API.WHvSetPartitionProperty(
          Partition, WHvPartitionPropertyCodeProcessorCount, &P, sizeof(P));
      FAILED(Status))
    return whpError(diagnostic::WhpCreate, Status,
                    whp::operation::WHvSetPartitionProperty);
  P = {};
  P.ExtendedVmExits.HypercallExit = 1;
  if (const auto Status = API.WHvSetPartitionProperty(
          Partition, WHvPartitionPropertyCodeExtendedVmExits, &P, sizeof(P));
      FAILED(Status))
    return whpError(diagnostic::WhpCreate, Status,
                    whp::operation::WHvSetPartitionProperty);
  P = {};
  P.Arm64IcParameters.EmulationMode = WHvArm64IcEmulationModeGicV3;
  auto &GIC = P.Arm64IcParameters.GicV3Parameters;
  GIC.GicdBaseAddress = aarch64::GicDistributor;
  GIC.GitsTranslaterBaseAddress = aarch64::GicITS;
  if (const auto Status = API.WHvGetCapability(WHvCapabilityCodeGicLpiIntIdBits,
                                               &C, sizeof(C), nullptr);
      FAILED(Status))
    return whpUnavailable(diagnostic::WhpArmCapability, Status,
                          whp::operation::WHvGetCapability);
  GIC.GicLpiIntIdBits = C.GicLpiIntIdBits;
  GIC.GicPpiOverflowInterruptFromCntv = aarch64::GicVirtualTimerPPI;
  GIC.GicPpiPerformanceMonitorsInterrupt = aarch64::GicPerformancePPI;
  if (const auto Status = API.WHvSetPartitionProperty(
          Partition, WHvPartitionPropertyCodeArm64IcParameters, &P, sizeof(P));
      FAILED(Status))
    return whpError(diagnostic::WhpCreate, Status,
                    whp::operation::WHvSetPartitionProperty);
  if (const auto Status = API.WHvSetupPartition(Partition); FAILED(Status))
    return whpError(diagnostic::WhpCreate, Status,
                    whp::operation::WHvSetupPartition);
  return llvm::Error::success();
}
llvm::Expected<std::unique_ptr<WhpVirtualProcessor>>
createWhpArmProcessor(MemoryProjection &Memory) {
  auto M = std::make_unique<WhpVirtualProcessor>();
  if (auto E = M->attach(Memory, configureWhpArmPartition))
    return E;
  WHV_REGISTER_NAME Name = WHvArm64RegisterGicrBaseGpa;
  WHV_REGISTER_VALUE Value{};
  Value.Reg64 = RedistributorBase + M->ProcessorIndex * RedistributorStride;
  if (const auto Status = M->API.WHvSetVirtualProcessorRegisters(
          M->Partition, M->ProcessorIndex, &Name, 1, &Value);
      FAILED(Status))
    return whpError(diagnostic::WhpState, Status,
                    whp::operation::WHvSetVirtualProcessorRegisters);
  if (auto E = M->initializeRunControl())
    return E;
  return std::unique_ptr<WhpVirtualProcessor>(std::move(M));
}
} // namespace
llvm::Expected<std::unique_ptr<AArch64Machine>>
createWhpAArch64Machine(MemoryProjection &Memory) {
  auto M = std::make_unique<WhpAArch64Machine>(Memory);
  if (auto E = M->initialize())
    return E;
  if (auto E = verifyAArch64Machine(*M, Memory))
    return E;
  return std::unique_ptr<AArch64Machine>(std::move(M));
}
} // namespace neverd::emulation
#else
namespace neverd::emulation {
llvm::Expected<std::unique_ptr<AArch64Machine>>
createWhpAArch64Machine(MemoryProjection &) {
  return diagnostic::unavailable(diagnostic::Unavailable);
}
} // namespace neverd::emulation
#endif
