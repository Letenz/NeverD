//===- WhpX64Partition.h - Captured x64 state owned by a WHP partition
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WHP_X64PARTITION_H
#define NEVERD_EMULATION_WHP_X64PARTITION_H

#include "../../arch/x86_64/X64Exception.h"
#include "WhpXsaveState.h"

#include <algorithm>
#include <array>
#include <utility>

namespace neverd::emulation {
/// Reuse belongs to this actual native partition, never to a logical CPU.
/// Every successful step captures all defined register fields and FP/SSE.
/// Only an acknowledged debug exit authorizes omission of unchanged inputs.
class WhpX64Partition final : public WhpPartition {
public:
  WhpXsaveState Xsave;

  /// Control already includes preparation and acquisition of the partition.
  llvm::Error step(X64MachineState &State, uint64_t Root,
                   MachineRunControl Control,
                   uint32_t MXCSRMask = x64::fp::BaselineMXCSRMask) {
    const bool Reuse = std::exchange(Runnable, false);
    if (auto E = validateX64FPState(State.FP))
      return E;
    const auto Input = registers(State, Root);
    if (auto E = installRegisters(Input, Reuse))
      return E;
    if (!Reuse || State.FP != CapturedState.FP ||
        State.MXCSR != CapturedState.MXCSR || State.Xmm != CapturedState.Xmm)
      if (auto E = Xsave.install(API, Partition, State, MXCSRMask))
        return E;
    WHV_RUN_VP_EXIT_CONTEXT Exit{};
    CapturePacket Actual{};
    auto Next = State;
    auto Complete = [&]() -> llvm::Error {
      if (Exit.ExitReason != WHvRunVpExitReasonException)
        return diagnostic::error(diagnostic::WhpExit);
      // Read control and segment state too. The next comparison uses the
      // actual host values, not an assumption that prior inputs survived.
      if (const auto Status = API.WHvGetVirtualProcessorRegisters(
              Partition, 0, CaptureNames.data(), CaptureNames.size(),
              Actual.data());
          FAILED(Status))
        return whpError(diagnostic::WhpState, Status,
                        whp::operation::WHvGetVirtualProcessorRegisters);
      size_t Index = 0;
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  Next.reg(X64Register::Name) = Actual[Index++].Reg64;
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
      WhpXsaveState::MetadataPacket Metadata{};
      std::copy(Actual.begin() + std::size(Names), Actual.end(),
                Metadata.begin());
      if (auto E = Xsave.capture(API, Partition, Next, &Metadata, MXCSRMask))
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
    CapturedState = Next;
    std::copy_n(Actual.begin(), CapturedRegisters.size(),
                CapturedRegisters.begin());
    Runnable = true;
    return llvm::Error::success();
  }

private:
  inline static constexpr WHV_REGISTER_NAME Names[] = {
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP) WHvX64Register##WHP,
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
#define NEVERD_WHP_X64_CONTROL(Name, Value) WHvX64Register##Name,
#define NEVERD_WHP_X64_SEGMENT(Name, Code, Base) WHvX64Register##Name,
#include "WhpX64Registers.def"
#undef NEVERD_WHP_X64_SEGMENT
#undef NEVERD_WHP_X64_CONTROL
  };
  inline static constexpr size_t SegmentCount = 0
#define NEVERD_WHP_X64_SEGMENT(Name, Code, Base) +1
#include "WhpX64Registers.def"
#undef NEVERD_WHP_X64_SEGMENT
      ;
  using RegisterPacket = std::array<WHV_REGISTER_VALUE, std::size(Names)>;
  inline static constexpr auto CaptureNames = [] {
    std::array<WHV_REGISTER_NAME,
               std::size(Names) + std::size(WhpXsaveState::MetadataNames)>
        Result{};
    auto End = std::copy(std::begin(Names), std::end(Names), Result.begin());
    std::copy(std::begin(WhpXsaveState::MetadataNames),
              std::end(WhpXsaveState::MetadataNames), End);
    return Result;
  }();
  using CapturePacket = std::array<WHV_REGISTER_VALUE, CaptureNames.size()>;

  static RegisterPacket registers(const X64MachineState &State, uint64_t Root) {
    RegisterPacket Values{};
    size_t Index = 0;
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  Values[Index++].Reg64 =                                                      \
      State.reg(X64Register::Name) |                                           \
      (X64Register::Name == X64Register::FLAGS ? x64::TrapFlag : 0);
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
#define NEVERD_WHP_X64_CONTROL(Name, Value) Values[Index++].Reg64 = Value;
#include "WhpX64Registers.def"
#undef NEVERD_WHP_X64_CONTROL
    auto AddSegment = [&](bool Code, uint64_t Base) {
      auto &S = Values[Index++].Segment;
      S.Selector = State.UserMode
                       ? (Code ? x64::UserCodeSelector : x64::UserDataSelector)
                       : (Code ? x64::CodeSelector : x64::DataSelector);
      S.DescriptorPrivilegeLevel = State.UserMode ? x64::UserPrivilege : 0;
      S.Base = Base;
      S.Limit = x64::SegmentLimit;
      S.Present = S.NonSystemSegment = S.Granularity = 1;
      S.SegmentType = Code ? x64::CodeType : x64::DataType;
      S.Long = Code;
      S.Default = !Code;
    };
#define NEVERD_WHP_X64_SEGMENT(Name, Code, Base) AddSegment(Code, Base);
#include "WhpX64Registers.def"
#undef NEVERD_WHP_X64_SEGMENT
    return Values;
  }

  static bool sameValue(size_t Index, const WHV_REGISTER_VALUE &L,
                        const WHV_REGISTER_VALUE &R) {
    if (Index < std::size(Names) - SegmentCount)
      return L.Reg64 == R.Reg64;
    // Do not compare union padding or reserved segment attribute bits.
#define NEVERD_WHP_X64_SEGMENT_FIELD(Field)                                    \
  if (L.Segment.Field != R.Segment.Field)                                      \
    return false;
#include "WhpX64Registers.def"
#undef NEVERD_WHP_X64_SEGMENT_FIELD
    return true;
  }

  llvm::Error installRegisters(const RegisterPacket &Input, bool Reuse) {
    std::array<WHV_REGISTER_NAME, std::size(Names)> ChangedNames{};
    RegisterPacket ChangedValues{};
    size_t Count = 0;
    for (size_t Index = 0; Index < Input.size(); ++Index) {
      if (Reuse && sameValue(Index, Input[Index], CapturedRegisters[Index]))
        continue;
      ChangedNames[Count] = Names[Index];
      ChangedValues[Count++] = Input[Index];
    }
    if (!Count)
      return llvm::Error::success();
    const auto Status = API.WHvSetVirtualProcessorRegisters(
        Partition, 0, ChangedNames.data(), Count, ChangedValues.data());
    return FAILED(Status)
               ? whpError(diagnostic::WhpState, Status,
                          whp::operation::WHvSetVirtualProcessorRegisters)
               : llvm::Error::success();
  }

  RegisterPacket CapturedRegisters{};
  X64MachineState CapturedState;
  bool Runnable = false;
};
} // namespace neverd::emulation
#endif
