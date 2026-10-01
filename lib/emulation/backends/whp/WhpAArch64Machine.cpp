//===- WhpAArch64Machine.cpp - Windows ARM64 checked execution ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/aarch64/AArch64GeneralState.h"
#include "../../core/ExecutionDiagnostics.h"
#include "../MachineFactories.h"
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__)) &&          \
    defined(NEVERD_EMULATION_WHP)
#include "WhpPartition.h"

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
class WhpAArch64Machine final : public AArch64Machine, public WhpPartition {
public:
  llvm::Error step(AArch64MachineState &State,
                   MachineRunControl Control) override {
    Control = Control.forNativeStep();
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
    Add(WHvArm64RegisterTtbr0El1, LowRoot);
    Add(WHvArm64RegisterTtbr1El1, HighRoot);
    Add(WHvArm64RegisterTcrEl1, TCR);
    Add(WHvArm64RegisterMairEl1, MAIR);
    Add(WHvArm64RegisterSctlrEl1, SCTLR);
    Add(WHvArm64RegisterVbarEl1, VectorGPA);
    Add(WHvArm64RegisterCpacrEl1, CPACR);
    if (FAILED(API.WHvSetVirtualProcessorRegisters(
            Partition, 0, Names.data(), Names.size(), Values.data())))
      return diagnostic::error(diagnostic::WhpState);
    // ARM64 WHP does not expose x64's exception-exit bitmap. The immutable EL1
    // vector gateway returns through an intercepted HVC after one debug step.
    WHV_RUN_VP_EXIT_CONTEXT Exit{};
    if (auto E = run(Exit, Control))
      return E;
    if (Exit.ExitReason != WHvRunVpExitReasonHypercall ||
        Exit.Hypercall.Header.Pc !=
            VectorGPA + (State.UserMode ? LowerELVector : CurrentELVector) ||
        Exit.Hypercall.Immediate)
      return diagnostic::error(diagnostic::WhpExit);
    Names.resize(StateCount);
    Values.resize(StateCount);
    const auto Syndrome = Add(WHvArm64RegisterEsrEl1, 0);
    if (FAILED(API.WHvGetVirtualProcessorRegisters(
            Partition, 0, Names.data(), Names.size(), Values.data())))
      return diagnostic::error(diagnostic::WhpState);
    if (((Values[Syndrome].Reg64 >> ExceptionClassShift) &
         ExceptionClassMask) !=
        (State.UserMode ? StepFromLowerEL : StepFromEL1))
      return diagnostic::error(diagnostic::ArmState);
    auto Next = State;
    if (auto E = captureAArch64State(
            Next,
            [&](AArch64Register Register) -> llvm::Expected<uint64_t> {
              return Values[unsigned(Register)].Reg64;
            },
            [&](unsigned Index) -> llvm::Expected<RegisterValue> {
              const auto &Value = Values[VectorBegin + Index].Reg128;
              return RegisterValue{Value.Low64, Value.High64};
            }))
      return E;
    if (Control.interrupted())
      return diagnostic::interrupted(diagnostic::WhpRun, Control);
    State = Next;
    return llvm::Error::success();
  }
};
} // namespace
llvm::Expected<std::unique_ptr<AArch64Machine>>
createWhpAArch64Machine(MemoryProjection &Memory) {
  auto M = std::make_unique<WhpAArch64Machine>();
  if (auto E = M->API.load())
    return E;
  WHV_CAPABILITY C{};
  if (FAILED(M->API.WHvGetCapability(WHvCapabilityCodeHypervisorPresent, &C,
                                     sizeof(C), nullptr)) ||
      !C.HypervisorPresent)
    return diagnostic::unavailable(diagnostic::WhpArmCapability,
                                   BackendAvailability::MissingCapability);
  if (FAILED(M->API.WHvGetCapability(WHvCapabilityCodeExtendedVmExits, &C,
                                     sizeof(C), nullptr)) ||
      !C.ExtendedVmExits.HypercallExit)
    return diagnostic::unavailable(diagnostic::WhpArmCapability,
                                   BackendAvailability::MissingCapability);
  if (FAILED(M->API.WHvCreatePartition(&M->Partition)))
    return diagnostic::error(diagnostic::WhpCreate);
  WHV_PARTITION_PROPERTY P{};
  P.ProcessorCount = 1;
  if (FAILED(M->API.WHvSetPartitionProperty(
          M->Partition, WHvPartitionPropertyCodeProcessorCount, &P, sizeof(P))))
    return diagnostic::error(diagnostic::WhpCreate);
  P = {};
  P.ExtendedVmExits.HypercallExit = 1;
  if (FAILED(M->API.WHvSetPartitionProperty(
          M->Partition, WHvPartitionPropertyCodeExtendedVmExits, &P,
          sizeof(P))))
    return diagnostic::error(diagnostic::WhpCreate);
  P = {};
  P.Arm64IcParameters.EmulationMode = WHvArm64IcEmulationModeGicV3;
  auto &GIC = P.Arm64IcParameters.GicV3Parameters;
  GIC.GicdBaseAddress = aarch64::GicDistributor;
  GIC.GitsTranslaterBaseAddress = aarch64::GicITS;
  if (FAILED(M->API.WHvGetCapability(WHvCapabilityCodeGicLpiIntIdBits, &C,
                                     sizeof(C), nullptr)))
    return diagnostic::unavailable(diagnostic::WhpArmCapability,
                                   BackendAvailability::MissingCapability);
  GIC.GicLpiIntIdBits = C.GicLpiIntIdBits;
  GIC.GicPpiOverflowInterruptFromCntv = aarch64::GicVirtualTimerPPI;
  GIC.GicPpiPerformanceMonitorsInterrupt = aarch64::GicPerformancePPI;
  if (FAILED(M->API.WHvSetPartitionProperty(
          M->Partition, WHvPartitionPropertyCodeArm64IcParameters, &P,
          sizeof(P))) ||
      FAILED(M->API.WHvSetupPartition(M->Partition)))
    return diagnostic::error(diagnostic::WhpCreate);
  for (const auto &Mapping : Memory.registrations())
    if (FAILED(M->API.WHvMapGpaRange(
            M->Partition, Mapping.Backing, Mapping.Physical, Mapping.Size,
            WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite |
                WHvMapGpaRangeFlagExecute)))
      return diagnostic::error(diagnostic::WhpMap);
  if (FAILED(M->API.WHvCreateVirtualProcessor(M->Partition, 0, 0)))
    return diagnostic::error(diagnostic::WhpCreate);
  WHV_REGISTER_NAME Name = WHvArm64RegisterGicrBaseGpa;
  WHV_REGISTER_VALUE Value{};
  Value.Reg64 = aarch64::GicRedistributor;
  if (FAILED(M->API.WHvSetVirtualProcessorRegisters(M->Partition, 0, &Name, 1,
                                                    &Value)))
    return diagnostic::error(diagnostic::WhpState);
  if (auto E = M->initializeRunControl())
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
