//===- WhpAArch64Machine.cpp - Windows ARM64 checked execution ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/aarch64/AArch64Machine.h"
#include "../../core/ExecutionDiagnostics.h"
#include "../MachineFactories.h"
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__)) &&          \
    defined(NEVERD_EMULATION_WHP)
#include "WhpPartition.h"

#include <vector>

namespace neverd::emulation {
namespace {
class WhpAArch64Machine final : public AArch64Machine, public WhpPartition {
public:
  llvm::Error step(AArch64MachineState &State,
                   MachineRunControl Control) override {
    using namespace aarch64;
    std::vector<WHV_REGISTER_NAME> Names;
    std::vector<WHV_REGISTER_VALUE> Values;
    auto Add = [&](WHV_REGISTER_NAME Name, uint64_t Value) {
      WHV_REGISTER_VALUE V{};
      V.Reg64 = Value;
      Names.push_back(Name);
      Values.push_back(V);
    };
    for (unsigned N = 0; N < GPRCount; ++N)
      Add(WHV_REGISTER_NAME(WHvArm64RegisterX0 + N), State.Registers[N]);
    Add(State.UserMode ? WHvArm64RegisterSpEl0 : WHvArm64RegisterSpEl1,
        State.reg(AArch64Register::SP));
    const uint64_t Mode = State.UserMode ? PStateEL0t : PStateEL1h;
    const unsigned GeneralCount = Names.size();
    Add(WHvArm64RegisterPc, EntryGPA);
    Add(WHvArm64RegisterPstate, PStateEL1h | PStateDAIF);
    Add(WHvArm64RegisterElrEl1, State.reg(AArch64Register::PC));
    Add(WHvArm64RegisterSpsrEl1, State.reg(AArch64Register::NZCV) | Mode |
                                     (PStateDAIF & ~PStateDebugMask) |
                                     PStateSingleStep);
    Add(WHvArm64RegisterMdscrEl1,
        MDSCRSingleStep | MDSCRKernelDebug | MDSCRMonitorDebug);
    Add(WHvArm64RegisterTtbr0El1, LowRoot);
    Add(WHvArm64RegisterTtbr1El1, HighRoot);
    Add(WHvArm64RegisterTcrEl1, TCR);
    Add(WHvArm64RegisterMairEl1, MAIR);
    Add(WHvArm64RegisterSctlrEl1, SCTLR);
    Add(WHvArm64RegisterVbarEl1, VectorGPA);
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
    Names.resize(GeneralCount);
    Values.resize(GeneralCount);
    Add(WHvArm64RegisterEsrEl1, 0);
    Add(WHvArm64RegisterElrEl1, 0);
    Add(WHvArm64RegisterSpsrEl1, 0);
    if (FAILED(API.WHvGetVirtualProcessorRegisters(
            Partition, 0, Names.data(), Names.size(), Values.data())))
      return diagnostic::error(diagnostic::WhpState);
    if (((Values[GeneralCount].Reg64 >> ExceptionClassShift) &
         ExceptionClassMask) !=
        (State.UserMode ? StepFromLowerEL : StepFromEL1))
      return diagnostic::error(diagnostic::ArmState);
    for (unsigned N = 0; N < GPRCount; ++N)
      State.Registers[N] = Values[N].Reg64;
    State.reg(AArch64Register::SP) = Values[GPRCount].Reg64;
    State.reg(AArch64Register::PC) = Values[GeneralCount + 1].Reg64;
    State.reg(AArch64Register::NZCV) =
        Values[GeneralCount + 2].Reg64 & NZCVMask;
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
