//===- WhpMachine.cpp - Windows x64 execution----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/x86_64/X64Exception.h"
#include "../../arch/x86_64/X64Machine.h"
#include "../../arch/x86_64/X64MachineProbe.h"
#include "../../core/ExecutionDiagnostics.h"
#include "../../core/MemoryProjection.h"
#include "../MachineFactories.h"
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__)) &&             \
    defined(NEVERD_EMULATION_WHP)
#include "WhpXsaveState.h"

#include <vector>
#include <windows.h>

namespace neverd::emulation {
namespace {
class WhpMachine final : public X64Machine, public WhpPartition {
public:
  WhpXsaveState Xsave;
  llvm::Error step(X64MachineState &State, uint64_t Root,
                   MachineRunControl Control) override {
    Control = Control.forNativeStep();
    if (auto E = validateX64FPState(State.FP))
      return E;
    std::vector<WHV_REGISTER_NAME> Names;
    std::vector<WHV_REGISTER_VALUE> Values;
    auto Add = [&](WHV_REGISTER_NAME Name, uint64_t Value) {
      WHV_REGISTER_VALUE V{};
      V.Reg64 = Value;
      Names.push_back(Name);
      Values.push_back(V);
    };
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  Add(WHvX64Register##WHP, State.reg(X64Register::Name));
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
    // The admitted ISA excludes all instructions observing or modifying TF.
    Values.back().Reg64 |= x64::TrapFlag;
    const size_t ObservableCount = Names.size();
    Add(WHvX64RegisterCr0, x64::CR0);
    Add(WHvX64RegisterCr3, Root);
    Add(WHvX64RegisterCr4, x64::CR4 | x64::fp::OSXsave);
    Add(WHvX64RegisterXCr0, x64::fp::FPAndSSE);
    Add(WHvX64RegisterEfer, x64::EFER);
    Add(WHvX64RegisterCr8, State.reg(X64Register::CR8));
    for (auto Name : {WHvX64RegisterCs, WHvX64RegisterSs, WHvX64RegisterDs,
                      WHvX64RegisterEs, WHvX64RegisterFs, WHvX64RegisterGs}) {
      WHV_REGISTER_VALUE V{};
      const bool Code = Name == WHvX64RegisterCs;
      V.Segment.Selector =
          State.UserMode
              ? (Code ? x64::UserCodeSelector : x64::UserDataSelector)
              : (Code ? x64::CodeSelector : x64::DataSelector);
      V.Segment.DescriptorPrivilegeLevel =
          State.UserMode ? x64::UserPrivilege : 0;
      V.Segment.Limit = x64::SegmentLimit;
      V.Segment.Present = V.Segment.NonSystemSegment = V.Segment.Granularity =
          1;
      V.Segment.SegmentType = Code ? x64::CodeType : x64::DataType;
      V.Segment.Long = Code;
      V.Segment.Default = !Code;
      if (Name == WHvX64RegisterGs)
        V.Segment.Base = State.GSBase;
      if (Name == WHvX64RegisterFs)
        V.Segment.Base = State.FSBase;
      Names.push_back(Name);
      Values.push_back(V);
    }
    if (FAILED(API.WHvSetVirtualProcessorRegisters(
            Partition, 0, Names.data(), Names.size(), Values.data())))
      return diagnostic::error(diagnostic::WhpState);
    if (auto E = Xsave.install(API, Partition, State))
      return E;
    WHV_RUN_VP_EXIT_CONTEXT Exit{};
    auto Next = State;
    auto Complete = [&]() -> llvm::Error {
      if (Exit.ExitReason != WHvRunVpExitReasonException)
        return diagnostic::error(diagnostic::WhpExit);
      if (FAILED(API.WHvGetVirtualProcessorRegisters(
              Partition, 0, Names.data(), ObservableCount, Values.data())))
        return diagnostic::error(diagnostic::WhpState);
      size_t I = 0;
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  Next.reg(X64Register::Name) = Values[I++].Reg64;
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
      if (auto E = Xsave.capture(API, Partition, Next))
        return E;
      Next.reg(X64Register::FLAGS) &= ~x64::TrapFlag;
      const unsigned Vector = Exit.VpException.ExceptionType;
      if (Vector != x64::DebugVector) {
        if (!x64::isExceptionVector(Vector) ||
            !(x64::ExceptionExitBitmap & (uint64_t(1) << Vector)))
          return diagnostic::error(diagnostic::WhpExit);
        const uint64_t Instrumentation = x64::TrapFlag | x64::ResumeFlag;
        Next.reg(X64Register::FLAGS) =
            (Next.reg(X64Register::FLAGS) & ~Instrumentation) |
            (State.reg(X64Register::FLAGS) & Instrumentation);
        State = Next;
        return llvm::make_error<X64ExceptionError>(X64Exception{
            Vector,
            Exit.VpException.ExceptionInfo.ErrorCodeValid
                ? std::optional<uint64_t>(Exit.VpException.ErrorCode)
                : std::nullopt,
            Vector == unsigned(x64::ExceptionVector::PageFault)
                ? std::optional<uint64_t>(Exit.VpException.ExceptionParameter)
                : std::nullopt});
      }
      return llvm::Error::success();
    };
    if (auto E = run(Exit, Control, Complete))
      return E;
    if (Control.interrupted())
      return diagnostic::interrupted(diagnostic::WhpRun, Control);
    State = Next;
    return llvm::Error::success();
  }
};
} // namespace
llvm::Expected<std::unique_ptr<X64Machine>>
createWhpMachine(MemoryProjection &Memory) {
  auto M = std::make_unique<WhpMachine>();
  if (auto E = M->API.load())
    return E;
  WHV_CAPABILITY C{};
  if (FAILED(M->API.WHvGetCapability(WHvCapabilityCodeHypervisorPresent, &C,
                                     sizeof(C), nullptr)) ||
      !C.HypervisorPresent)
    return diagnostic::unavailable(diagnostic::WhpCapability,
                                   BackendAvailability::MissingCapability);
  if (FAILED(M->API.WHvGetCapability(WHvCapabilityCodeExtendedVmExits, &C,
                                     sizeof(C), nullptr)) ||
      !C.ExtendedVmExits.ExceptionExit)
    return diagnostic::unavailable(diagnostic::WhpCapability,
                                   BackendAvailability::MissingCapability);
  if (FAILED(M->API.WHvGetCapability(WHvCapabilityCodeExceptionExitBitmap, &C,
                                     sizeof(C), nullptr)) ||
      (C.ExceptionExitBitmap & x64::ExceptionExitBitmap) !=
          x64::ExceptionExitBitmap)
    return diagnostic::unavailable(diagnostic::WhpCapability,
                                   BackendAvailability::MissingCapability);
  if (FAILED(M->API.WHvGetCapability(WHvCapabilityCodeProcessorXsaveFeatures,
                                     &C, sizeof(C), nullptr)) ||
      !C.ProcessorXsaveFeatures.XsaveSupport)
    return diagnostic::unavailable(diagnostic::WhpCapability,
                                   BackendAvailability::MissingCapability);
  if (FAILED(M->API.WHvCreatePartition(&M->Partition)))
    return diagnostic::error(diagnostic::WhpCreate);
  WHV_PARTITION_PROPERTY P{};
  P.ProcessorCount = 1;
  if (FAILED(M->API.WHvSetPartitionProperty(
          M->Partition, WHvPartitionPropertyCodeProcessorCount, &P, sizeof(P))))
    return diagnostic::error(diagnostic::WhpCreate);
  P = {};
  P.ExtendedVmExits.ExceptionExit = 1;
  if (FAILED(M->API.WHvSetPartitionProperty(
          M->Partition, WHvPartitionPropertyCodeExtendedVmExits, &P,
          sizeof(P))))
    return diagnostic::error(diagnostic::WhpCreate);
  P = {};
  P.ProcessorXsaveFeatures.XsaveSupport = 1;
  if (FAILED(M->API.WHvSetPartitionProperty(
          M->Partition, WHvPartitionPropertyCodeProcessorXsaveFeatures, &P,
          sizeof(P))))
    return diagnostic::error(diagnostic::WhpCreate);
  P = {};
  P.ExceptionExitBitmap = x64::ExceptionExitBitmap;
  if (FAILED(M->API.WHvSetPartitionProperty(
          M->Partition, WHvPartitionPropertyCodeExceptionExitBitmap, &P,
          sizeof(P))) ||
      FAILED(M->API.WHvSetupPartition(M->Partition)))
    return diagnostic::error(diagnostic::WhpCreate);
  if (auto E = M->mapMemory(Memory))
    return E;
  if (FAILED(M->API.WHvCreateVirtualProcessor(M->Partition, 0, 0)))
    return diagnostic::error(diagnostic::WhpCreate);
  if (auto E = M->Xsave.initialize(M->API, M->Partition))
    return E;
  if (auto E = M->initializeRunControl())
    return E;
  if (auto E = verifyX64Machine(*M, Memory))
    return E;
  return std::unique_ptr<X64Machine>(std::move(M));
}
} // namespace neverd::emulation
#else

namespace neverd::emulation {
llvm::Expected<std::unique_ptr<X64Machine>>
createWhpMachine(MemoryProjection &) {
  return diagnostic::unavailable(diagnostic::Unavailable);
}
} // namespace neverd::emulation
#endif
