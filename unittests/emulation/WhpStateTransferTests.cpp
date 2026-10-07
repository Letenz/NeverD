//===- WhpStateTransferTests.cpp - WHP partition state reuse and failure
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__)) &&             \
    defined(NEVERD_EMULATION_WHP)
#include "backends/whp/WhpX64Processor.h"
#include "gtest/gtest.h"

#include "llvm/Support/Endian.h"

#include <cstring>
#include <map>
#include <optional>
#include <vector>

namespace neverd::emulation {
namespace {
#define NEVERD_WHP_TRANSFER_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "WhpStateTransferCases.def"
#undef NEVERD_WHP_TRANSFER_VALUE
enum class Stage {
  None,
#define NEVERD_WHP_TRANSFER_FAILURE(Name) Name,
#include "WhpStateTransferCases.def"
#undef NEVERD_WHP_TRANSFER_FAILURE
};
constexpr Stage Failures[] = {
#define NEVERD_WHP_TRANSFER_FAILURE(Name) Stage::Name,
#include "WhpStateTransferCases.def"
#undef NEVERD_WHP_TRANSFER_FAILURE
};
constexpr Stage Stops[] = {
#define NEVERD_WHP_TRANSFER_STOP(Name) Stage::Name,
#include "WhpStateTransferCases.def"
#undef NEVERD_WHP_TRANSFER_STOP
};
struct MetadataCorruption {
  WHV_REGISTER_NAME Register;
  void (*Apply)(WHV_REGISTER_VALUE &);
};
constexpr MetadataCorruption Corruptions[] = {
#define NEVERD_WHP_TRANSFER_METADATA(Register, Field)                          \
  {WHvX64Register##Register,                                                   \
   [](WHV_REGISTER_VALUE &Value) { --Value.Register.Field; }},
#include "WhpStateTransferCases.def"
#undef NEVERD_WHP_TRANSFER_METADATA
};
// This host models API transfers and partial failures, not processor semantics.
// Native X64StateTransition/FP/exception suites separately run the real WHP
// CPU.
struct HostState {
  std::map<WHV_REGISTER_NAME, WHV_REGISTER_VALUE> Registers;
  std::array<uint8_t, x64::fp::XsaveBytes> Xsave{};
  std::vector<WHV_REGISTER_NAME> LastInstalled;
  std::vector<WHV_REGISTER_NAME> LastCaptured;
  std::atomic<bool> Stop{false};
  Stage Failure = Stage::None, StopAt = Stage::None;
  unsigned RegisterInstalls = 0, XsaveInstalls = 0, MetadataInstalls = 0;
  unsigned RegisterCaptures = 0, XsaveCaptures = 0, MetadataCaptures = 0;
  unsigned CaptureCalls = 0;
  std::optional<unsigned> CaptureFailureAt;
  const MetadataCorruption *CorruptMetadata = nullptr;
  unsigned Entries = 0, Exception = x64::DebugVector;
  bool ChangeControls = false, PoisonPadding = false,
       OmitPacketMetadata = false, CancelledExit = false;

  bool boundary(Stage Current) {
    if (Current == StopAt)
      Stop = true;
    return Current == Failure;
  }
  static HRESULT WINAPI property(WHV_PARTITION_HANDLE,
                                 WHV_PARTITION_PROPERTY_CODE, VOID *Bytes,
                                 UINT32 Size, UINT32 *Written) {
    EXPECT_EQ(Size, sizeof(WHV_PROCESSOR_XSAVE_FEATURES));
    WHV_PROCESSOR_XSAVE_FEATURES Features{};
    Features.XsaveSupport = 1;
    std::memcpy(Bytes, &Features, sizeof(Features));
    *Written = sizeof(Features);
    return S_OK;
  }
  static HRESULT WINAPI setRegisters(WHV_PARTITION_HANDLE Handle, UINT32 CPU,
                                     const WHV_REGISTER_NAME *Names,
                                     UINT32 Count,
                                     const WHV_REGISTER_VALUE *Values) {
    EXPECT_EQ(CPU, ProcessorIndex);
    auto &Self = *static_cast<HostState *>(Handle);
    const bool Metadata = Names[0] == WHvX64RegisterFpControlStatus;
    ++(Metadata ? Self.MetadataInstalls : Self.RegisterInstalls);
    if (!Metadata)
      Self.LastInstalled.assign(Names, Names + Count);
    for (unsigned I = 0; I < Count; ++I) {
      Self.Registers[Names[I]] = Values[I];
      // A failed batch may already have installed an arbitrary prefix.
      if (Self.boundary(Metadata ? Stage::MetadataInstall
                                 : Stage::RegisterInstall))
        return E_FAIL;
    }
    return S_OK;
  }
  static HRESULT WINAPI getRegisters(WHV_PARTITION_HANDLE Handle, UINT32 CPU,
                                     const WHV_REGISTER_NAME *Names,
                                     UINT32 Count, WHV_REGISTER_VALUE *Values) {
    EXPECT_EQ(CPU, ProcessorIndex);
    auto &Self = *static_cast<HostState *>(Handle);
    ++Self.CaptureCalls;
    Self.LastCaptured.assign(Names, Names + Count);
    if (Names[0] != WHvX64RegisterFpControlStatus)
      ++Self.RegisterCaptures;
    for (unsigned I = 0; I < Count; ++I) {
      const bool Metadata = Names[I] == WHvX64RegisterFpControlStatus ||
                            Names[I] == WHvX64RegisterXmmControlStatus;
      if (Names[I] == WHvX64RegisterFpControlStatus)
        ++Self.MetadataCaptures;
      Values[I] = Self.Registers.at(Names[I]);
      if (Self.CorruptMetadata && Self.CorruptMetadata->Register == Names[I])
        Self.CorruptMetadata->Apply(Values[I]);
      if (Self.PoisonPadding && !Metadata) {
        if (Names[I] >= WHvX64RegisterEs && Names[I] <= WHvX64RegisterGs)
          Values[I].Segment.Reserved = ReservedSegmentBits;
        else
          Values[I].Reg128.High64 = UnionPadding;
      }
      if (Self.CaptureFailureAt == I ||
          Self.boundary(Metadata ? Stage::MetadataCapture
                                 : Stage::RegisterCapture))
        return E_FAIL;
    }
    return S_OK;
  }
  static HRESULT WINAPI getXsave(WHV_PARTITION_HANDLE Handle, UINT32 CPU,
                                 VOID *Bytes, UINT32 Size, UINT32 *Written) {
    EXPECT_EQ(CPU, ProcessorIndex);
    auto &Self = *static_cast<HostState *>(Handle);
    *Written = Self.Xsave.size();
    if (!Size)
      return HRESULT(InsufficientBuffer);
    ++Self.XsaveCaptures;
    EXPECT_GE(Size, Self.Xsave.size());
    std::memcpy(Bytes, Self.Xsave.data(), Self.Xsave.size());
    if (Self.OmitPacketMetadata) {
      auto *Packet = static_cast<uint8_t *>(Bytes);
      llvm::support::endian::write16le(Packet + x64::fp::OpcodeOffset, 0);
      llvm::support::endian::write64le(Packet + x64::fp::InstructionOffset, 0);
      llvm::support::endian::write64le(Packet + x64::fp::DataOffset, 0);
    }
    return Self.boundary(Stage::XsaveCapture) ? E_FAIL : S_OK;
  }
  static HRESULT WINAPI setXsave(WHV_PARTITION_HANDLE Handle, UINT32 CPU,
                                 const VOID *Bytes, UINT32 Size) {
    EXPECT_EQ(CPU, ProcessorIndex);
    auto &Self = *static_cast<HostState *>(Handle);
    ++Self.XsaveInstalls;
    EXPECT_GE(Size, Self.Xsave.size());
    std::memcpy(Self.Xsave.data(), Bytes, Self.Xsave.size());
    return Self.boundary(Stage::XsaveInstall) ? E_FAIL : S_OK;
  }
  static HRESULT WINAPI getState(WHV_PARTITION_HANDLE Handle, UINT32 CPU,
                                 WHV_VIRTUAL_PROCESSOR_STATE_TYPE, VOID *Bytes,
                                 UINT32 Size, UINT32 *Written) {
    return getXsave(Handle, CPU, Bytes, Size, Written);
  }
  static HRESULT WINAPI setState(WHV_PARTITION_HANDLE Handle, UINT32 CPU,
                                 WHV_VIRTUAL_PROCESSOR_STATE_TYPE,
                                 const VOID *Bytes, UINT32 Size) {
    return setXsave(Handle, CPU, Bytes, Size);
  }
  static HRESULT WINAPI run(WHV_PARTITION_HANDLE Handle, UINT32 CPU,
                            VOID *Bytes, UINT32) {
    EXPECT_EQ(CPU, ProcessorIndex);
    auto &Self = *static_cast<HostState *>(Handle);
    ++Self.Entries;
    auto &Exit = *static_cast<WHV_RUN_VP_EXIT_CONTEXT *>(Bytes);
    Exit.ExitReason = Self.CancelledExit ? WHvRunVpExitReasonCanceled
                                         : WHvRunVpExitReasonException;
    Exit.VpException.ExceptionType = Self.Exception;
    if (Self.Exception == x64::DebugVector) {
      ++Self.Registers[WHvX64RegisterRax].Reg64;
      ++Self.Registers[WHvX64RegisterRip].Reg64;
      auto *Vector = Self.Xsave.data() + VectorOffset;
      llvm::support::endian::write64le(
          Vector, llvm::support::endian::read64le(Vector) + 1);
    }
    if (Self.ChangeControls) {
      Self.Registers[WHvX64RegisterCr3].Reg64 = OtherRoot;
      Self.Registers[WHvX64RegisterFs].Segment.Base = GSBase;
    }
    return Self.boundary(Stage::Entry) ? E_FAIL : S_OK;
  }
  static HRESULT WINAPI cancel(WHV_PARTITION_HANDLE, UINT32 CPU, UINT32) {
    EXPECT_EQ(CPU, ProcessorIndex);
    return S_OK;
  }
  static HRESULT WINAPI destroy(WHV_PARTITION_HANDLE) { return S_OK; }
};

class WhpStateTransfer : public testing::TestWithParam<bool> {
protected:
  HostState Target;
  std::unique_ptr<WhpX64Processor> Host;
  X64MachineState State;
  void initialize() {
    Host = std::make_unique<WhpX64Processor>();
    Host->Partition = &Target;
    Host->ProcessorIndex = ProcessorIndex;
    auto &API = Host->API;
    API.WHvDeletePartition = HostState::destroy;
    API.WHvGetPartitionProperty = HostState::property;
    API.WHvSetVirtualProcessorRegisters = HostState::setRegisters;
    API.WHvGetVirtualProcessorRegisters = HostState::getRegisters;
    API.WHvRunVirtualProcessor = HostState::run;
    API.WHvCancelRunVirtualProcessor = HostState::cancel;
    if (GetParam()) {
      API.WHvGetVirtualProcessorState = HostState::getState;
      API.WHvSetVirtualProcessorState = HostState::setState;
    } else {
      API.WHvGetVirtualProcessorXsaveState = HostState::getXsave;
      API.WHvSetVirtualProcessorXsaveState = HostState::setXsave;
    }
    ASSERT_EQ(llvm::toString(Host->Xsave.initialize(API, Host->Partition,
                                                    Host->ProcessorIndex)),
              "");
    ASSERT_EQ(llvm::toString(Host->initializeRunControl()), "");
  }
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(initialize());
    State.reg(X64Register::AX) = Integer;
    State.reg(X64Register::PC) = PC;
    State.reg(X64Register::FLAGS) = Flags;
    State.FSBase = FSBase;
    State.GSBase = GSBase;
    State.Xmm.front()[0] = Vector;
    State.FP.Opcode = FPOpcode;
    State.FP.Instruction = FPInstruction;
    State.FP.Data = FPData;
  }
  llvm::Error step(uint64_t PageRoot = Root) {
    return Host->step(
        State, PageRoot,
        MachineRunControl{std::chrono::steady_clock::time_point::max(),
                          &Target.Stop}
            .forNativeStep());
  }
  llvm::Error run() {
    return Host->run(
        State, Root,
        {std::chrono::steady_clock::time_point::max(), &Target.Stop});
  }
  static X64MachineState advanced(X64MachineState Before) {
    ++Before.reg(X64Register::AX);
    ++Before.reg(X64Register::PC);
    ++Before.Xmm.front()[0];
    return Before;
  }
  void expectFullRetry(const X64MachineState &Before) {
    const auto Installs = Target.XsaveInstalls;
    ASSERT_EQ(llvm::toString(step()), "");
    EXPECT_EQ(State, advanced(Before));
    EXPECT_EQ(Target.LastInstalled.size(), FullRegisterCount);
    EXPECT_EQ(Target.XsaveInstalls, Installs + 1);
  }
};

TEST_P(WhpStateTransfer, ContinuedStepsReuseCapturedRegistersAndFP) {
  Target.OmitPacketMetadata = true;
  const auto Initial = State;
  for (unsigned I = 0; I < Steps; ++I)
    ASSERT_EQ(llvm::toString(step()), "");
  EXPECT_EQ(Target.RegisterInstalls, 1u);
  EXPECT_EQ(Target.XsaveInstalls, 1u);
  EXPECT_EQ(Target.MetadataInstalls, 1u);
  EXPECT_EQ(Target.RegisterCaptures, Steps);
  EXPECT_EQ(Target.XsaveCaptures, Steps);
  EXPECT_EQ(Target.MetadataCaptures, Steps);
  EXPECT_EQ(Target.CaptureCalls, Steps);
  ASSERT_EQ(Target.LastCaptured.size(), FullCaptureCount);
  EXPECT_EQ(Target.LastCaptured[FullRegisterCount],
            WHvX64RegisterFpControlStatus);
  EXPECT_EQ(Target.LastCaptured.back(), WHvX64RegisterXmmControlStatus);
  EXPECT_EQ(State.reg(X64Register::AX), Integer + Steps);
  EXPECT_EQ(State.reg(X64Register::PC), PC + Steps);
  EXPECT_EQ(State.Xmm.front()[0], Vector + Steps);
  EXPECT_EQ(State.reg(X64Register::FLAGS), Initial.reg(X64Register::FLAGS));
  EXPECT_EQ(State.FP, Initial.FP);
}
TEST_P(WhpStateTransfer, HostChangesInstallOnlyChangedGroups) {
  ASSERT_EQ(llvm::toString(step()), "");
  State.reg(X64Register::AX) += InputChange;
  const auto Before = State;
  ASSERT_EQ(llvm::toString(step()), "");
  EXPECT_EQ(State, advanced(Before));
  EXPECT_EQ(Target.LastInstalled,
            (std::vector<WHV_REGISTER_NAME>{WHvX64RegisterRax}));
  EXPECT_EQ(Target.XsaveInstalls, 1u);
  State.FSBase = GSBase;
  State.reg(X64Register::CR8) = 1;
  ASSERT_EQ(llvm::toString(step(OtherRoot)), "");
  EXPECT_EQ(Target.LastInstalled,
            (std::vector<WHV_REGISTER_NAME>{
                WHvX64RegisterCr3, WHvX64RegisterCr8, WHvX64RegisterFs}));
  const auto Installs = Target.RegisterInstalls;
  State.MXCSR = MXCSR;
  State.FP.Control = FPControl;
  State.Xmm.back()[1] = Vector;
  ASSERT_EQ(llvm::toString(step(OtherRoot)), "");
  EXPECT_EQ(Target.RegisterInstalls, Installs);
  EXPECT_EQ(Target.XsaveInstalls, 2u);
  EXPECT_EQ(State.MXCSR, MXCSR);
  EXPECT_EQ(State.FP.Control, FPControl);
  EXPECT_EQ(State.Xmm.back()[1], Vector);
  State.UserMode = true;
  ASSERT_EQ(llvm::toString(step(OtherRoot)), "");
  EXPECT_EQ(Target.LastInstalled,
            (std::vector<WHV_REGISTER_NAME>{
                WHvX64RegisterCs, WHvX64RegisterSs, WHvX64RegisterDs,
                WHvX64RegisterEs, WHvX64RegisterFs, WHvX64RegisterGs}));
}
TEST_P(WhpStateTransfer, ActualControlCapturesOverrideOldInputAssumptions) {
  Target.ChangeControls = true;
  ASSERT_EQ(llvm::toString(step()), "");
  Target.ChangeControls = false;
  ASSERT_EQ(llvm::toString(step()), "");
  EXPECT_EQ(Target.LastInstalled, (std::vector<WHV_REGISTER_NAME>{
                                      WHvX64RegisterCr3, WHvX64RegisterFs}));
  EXPECT_EQ(Target.Registers.at(WHvX64RegisterCr3).Reg64, Root);
  EXPECT_EQ(Target.Registers.at(WHvX64RegisterFs).Segment.Base, FSBase);
}
TEST_P(WhpStateTransfer, ReservedBitsAndUnionPaddingDoNotForceReinstallation) {
  Target.PoisonPadding = true;
  ASSERT_EQ(llvm::toString(step()), "");
  const auto Before = State;
  ASSERT_EQ(llvm::toString(step()), "");
  EXPECT_EQ(State, advanced(Before));
  EXPECT_EQ(Target.RegisterInstalls, 1u);
  EXPECT_EQ(Target.XsaveInstalls, 1u);
}
TEST_P(WhpStateTransfer,
       PartialTransferFailuresPreserveStateAndForceFullRetry) {
  for (auto Failure : Failures) {
    SCOPED_TRACE(unsigned(Failure));
    ASSERT_EQ(llvm::toString(step()), "");
    State.reg(X64Register::AX) += InputChange;
    State.Xmm.back()[0] += InputChange;
    const auto Before = State;
    Target.Failure = Failure;
    auto E = step();
    EXPECT_TRUE(bool(E));
    EXPECT_FALSE(E.isA<MachineInterruptedError>());
    llvm::consumeError(std::move(E));
    EXPECT_EQ(State, Before);
    Target.Failure = Stage::None;
    ASSERT_NO_FATAL_FAILURE(expectFullRetry(Before));
  }
  // A failed combined read may have overwritten any prefix, including all
  // ordinary registers before failing while reading the FP metadata.
  for (unsigned Index = 0; Index < FullCaptureCount; ++Index) {
    SCOPED_TRACE(Index);
    ASSERT_EQ(llvm::toString(step()), "");
    const auto Before = State;
    const auto Captures = Target.XsaveCaptures;
    Target.CaptureFailureAt = Index;
    auto E = step();
    EXPECT_TRUE(bool(E));
    EXPECT_FALSE(E.isA<MachineInterruptedError>());
    llvm::consumeError(std::move(E));
    EXPECT_EQ(State, Before);
    EXPECT_EQ(Target.XsaveCaptures, Captures);
    Target.CaptureFailureAt.reset();
    ASSERT_NO_FATAL_FAILURE(expectFullRetry(Before));
  }
  for (const auto &Corruption : Corruptions) {
    SCOPED_TRACE(unsigned(Corruption.Register));
    ASSERT_EQ(llvm::toString(step()), "");
    const auto Before = State;
    Target.CorruptMetadata = &Corruption;
    auto E = step();
    EXPECT_TRUE(bool(E));
    EXPECT_FALSE(E.isA<MachineInterruptedError>());
    llvm::consumeError(std::move(E));
    EXPECT_EQ(State, Before);
    Target.CorruptMetadata = nullptr;
    ASSERT_NO_FATAL_FAILURE(expectFullRetry(Before));
  }
}
TEST_P(WhpStateTransfer, CancelledCapturePreservesStateAndForcesFullRetry) {
  for (auto Stop : Stops) {
    SCOPED_TRACE(unsigned(Stop));
    ASSERT_EQ(llvm::toString(step()), "");
    const auto Before = State;
    Target.StopAt = Stop;
    auto E = step();
    EXPECT_TRUE(E.isA<MachineInterruptedError>());
    llvm::consumeError(std::move(E));
    EXPECT_EQ(State, Before);
    Target.StopAt = Stage::None;
    Target.Stop = false;
    ASSERT_NO_FATAL_FAILURE(expectFullRetry(Before));
  }
}
TEST_P(WhpStateTransfer, CancelledDirectRunPublishesACompleteBoundary) {
  const auto Before = State;
  Target.CancelledExit = true;
  Target.StopAt = Stage::Entry;
  auto E = run();
  EXPECT_TRUE(E.isA<MachineInterruptedError>());
  llvm::consumeError(std::move(E));
  EXPECT_EQ(State, advanced(Before));
  Target.CancelledExit = false;
  Target.StopAt = Stage::None;
  Target.Stop = false;
  const auto Captured = State;
  ASSERT_NO_FATAL_FAILURE(expectFullRetry(Captured));
}
TEST_P(WhpStateTransfer, FailedDirectCapturePreservesStateAndForcesFullRetry) {
  auto Try = [&] {
    const auto Before = State;
    Target.CancelledExit = true;
    Target.StopAt = Stage::Entry;
    auto E = run();
    EXPECT_TRUE(bool(E));
    EXPECT_FALSE(E.isA<MachineInterruptedError>());
    llvm::consumeError(std::move(E));
    EXPECT_EQ(State, Before);
    Target.CancelledExit = false;
    Target.StopAt = Target.Failure = Stage::None;
    Target.Stop = false;
    Target.CaptureFailureAt.reset();
    Target.CorruptMetadata = nullptr;
    ASSERT_NO_FATAL_FAILURE(expectFullRetry(Before));
  };
  for (const auto Failure :
       {Stage::RegisterCapture, Stage::XsaveCapture, Stage::MetadataCapture}) {
    SCOPED_TRACE(unsigned(Failure));
    Target.Failure = Failure;
    ASSERT_NO_FATAL_FAILURE(Try());
  }
  for (unsigned I = 0; I < FullCaptureCount; ++I) {
    SCOPED_TRACE(I);
    Target.CaptureFailureAt = I;
    ASSERT_NO_FATAL_FAILURE(Try());
  }
  for (const auto &Corruption : Corruptions) {
    SCOPED_TRACE(unsigned(Corruption.Register));
    Target.CorruptMetadata = &Corruption;
    ASSERT_NO_FATAL_FAILURE(Try());
  }
}
TEST_P(WhpStateTransfer, ExceptionsOutrankCancellationAndInvalidateReuse) {
  for (auto Stop : Stops) {
    SCOPED_TRACE(unsigned(Stop));
    ASSERT_EQ(llvm::toString(step()), "");
    const auto Before = State;
    Target.Exception = unsigned(x64::ExceptionVector::Divide);
    Target.StopAt = Stop;
    auto E = step();
    EXPECT_TRUE(E.isA<X64ExceptionError>());
    llvm::consumeError(std::move(E));
    EXPECT_EQ(State, Before);
    Target.Exception = x64::DebugVector;
    Target.StopAt = Stage::None;
    Target.Stop = false;
    ASSERT_NO_FATAL_FAILURE(expectFullRetry(Before));
  }
}
TEST_P(WhpStateTransfer, ReplacementPartitionCannotReuseRetiredNativeState) {
  ASSERT_EQ(llvm::toString(step()), "");
  ASSERT_EQ(llvm::toString(step()), "");
  const auto Before = State;
  Host.reset();
  Target.Registers.clear();
  Target.Xsave.fill(0);
  ASSERT_NO_FATAL_FAILURE(initialize());
  ASSERT_NO_FATAL_FAILURE(expectFullRetry(Before));
}
INSTANTIATE_TEST_SUITE_P(StateAPIs, WhpStateTransfer, testing::Bool());
} // namespace
} // namespace neverd::emulation
#endif
