//===- WhpMachine.cpp - Windows x64 execution----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/x86_64/X64Exception.h"
#include "../../arch/x86_64/X64Machine.h"
#include "../../core/ExecutionDiagnostics.h"
#include "../../core/MemoryProjection.h"
#include "../MachineFactories.h"
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__)) &&             \
    defined(NEVERD_EMULATION_WHP)
#include "WhpPartition.h"

#include <vector>
#include <windows.h>

namespace neverd::emulation {
namespace {
class WhpMachine final : public X64Machine, public WhpPartition {
public:
  llvm::Error step(X64MachineState &State, uint64_t Root,
                   MachineRunControl Control) override {
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
    for (unsigned I = 0; I < State.Xmm.size(); ++I) {
      WHV_REGISTER_VALUE V{};
      V.Reg128.Low64 = State.Xmm[I][0];
      V.Reg128.High64 = State.Xmm[I][1];
      Names.push_back(static_cast<WHV_REGISTER_NAME>(WHvX64RegisterXmm0 + I));
      Values.push_back(V);
    }
    for (unsigned I = 0; I < State.FP.Registers.size(); ++I) {
      WHV_REGISTER_VALUE V{};
      V.Fp.AsUINT128.Low64 = State.FP.Registers[I][0];
      V.Fp.AsUINT128.High64 = State.FP.Registers[I][1];
      Names.push_back(static_cast<WHV_REGISTER_NAME>(WHvX64RegisterFpMmx0 + I));
      Values.push_back(V);
    }
    WHV_REGISTER_VALUE FP{};
    FP.FpControlStatus.FpControl = State.FP.Control;
    FP.FpControlStatus.FpStatus = State.FP.Status;
    FP.FpControlStatus.FpTag = State.FP.Tag;
    FP.FpControlStatus.LastFpOp = State.FP.Opcode;
    FP.FpControlStatus.LastFpRip = State.FP.Instruction;
    Names.push_back(WHvX64RegisterFpControlStatus);
    Values.push_back(FP);
    WHV_REGISTER_VALUE MXCSR{};
    MXCSR.XmmControlStatus.LastFpRdp = State.FP.Data;
    MXCSR.XmmControlStatus.XmmStatusControl = State.MXCSR;
    Names.push_back(WHvX64RegisterXmmControlStatus);
    Values.push_back(MXCSR);
    const size_t ObservableCount = Names.size();
    Add(WHvX64RegisterCr0, x64::CR0);
    Add(WHvX64RegisterCr3, Root);
    Add(WHvX64RegisterCr4, x64::CR4);
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
    WHV_RUN_VP_EXIT_CONTEXT Exit{};
    if (auto E = run(Exit, Control))
      return E;
    if (Exit.ExitReason != WHvRunVpExitReasonException)
      return diagnostic::error(diagnostic::WhpExit);
    if (FAILED(API.WHvGetVirtualProcessorRegisters(
            Partition, 0, Names.data(), ObservableCount, Values.data())))
      return diagnostic::error(diagnostic::WhpState);
    auto Next = State;
    size_t I = 0;
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  Next.reg(X64Register::Name) = Values[I++].Reg64;
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
    for (auto &Xmm : Next.Xmm) {
      Xmm = {Values[I].Reg128.Low64, Values[I].Reg128.High64};
      ++I;
    }
    for (auto &Register : Next.FP.Registers) {
      Register = {Values[I].Fp.AsUINT128.Low64,
                  Values[I].Fp.AsUINT128.High64 & x64::fp::RegisterHighMask};
      ++I;
    }
    const auto &FPNext = Values[I++].FpControlStatus;
    Next.FP.Control = FPNext.FpControl;
    Next.FP.Status = FPNext.FpStatus;
    Next.FP.Tag = FPNext.FpTag;
    Next.FP.Opcode = FPNext.LastFpOp;
    Next.FP.Instruction = FPNext.LastFpRip;
    Next.FP.Data = Values[I].XmmControlStatus.LastFpRdp;
    Next.MXCSR = Values[I].XmmControlStatus.XmmStatusControl;
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
  P.ExceptionExitBitmap = x64::ExceptionExitBitmap;
  if (FAILED(M->API.WHvSetPartitionProperty(
          M->Partition, WHvPartitionPropertyCodeExceptionExitBitmap, &P,
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
  if (auto E = M->initializeRunControl())
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
