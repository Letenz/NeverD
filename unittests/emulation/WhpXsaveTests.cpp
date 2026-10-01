//===- WhpXsaveTests.cpp - WHP complete-state packet protocol ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__)) &&             \
    defined(NEVERD_EMULATION_WHP)
#include "backends/whp/WhpXsaveState.h"
#include "gtest/gtest.h"

#include "llvm/Support/Endian.h"

#include <cstring>

namespace neverd::emulation {
namespace {
#define NEVERD_FP_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "X64FPCases.def"
#undef NEVERD_FP_VALUE
constexpr RegisterValue Payloads[] = {
#define NEVERD_FP_PAYLOAD(Low, High) {Low, High},
#include "X64FPCases.def"
#undef NEVERD_FP_PAYLOAD
};
struct FakeXsave {
  std::array<uint8_t, x64::fp::XsaveBytes> Packet{};
  UINT32 Required = Packet.size(), Written = Packet.size();
  unsigned Queries = 0, Installs = 0, Captures = 0;
  HRESULT CaptureStatus = S_OK;
  static HRESULT WINAPI get(WHV_PARTITION_HANDLE Handle, UINT32 Index,
                            VOID *Bytes, UINT32 Size, UINT32 *Written) {
    auto &Self = *static_cast<FakeXsave *>(Handle);
    EXPECT_EQ(Index, 0u);
    if (!Size) {
      ++Self.Queries;
      *Written = Self.Required;
      return HRESULT(InsufficientBuffer);
    }
    ++Self.Captures;
    if (FAILED(Self.CaptureStatus))
      return Self.CaptureStatus;
    EXPECT_GE(Size, Self.Packet.size());
    std::memcpy(Bytes, Self.Packet.data(), Self.Packet.size());
    *Written = Self.Written;
    return S_OK;
  }
  static HRESULT WINAPI set(WHV_PARTITION_HANDLE Handle, UINT32 Index,
                            const VOID *Bytes, UINT32 Size) {
    auto &Self = *static_cast<FakeXsave *>(Handle);
    EXPECT_EQ(Index, 0u);
    EXPECT_GE(Size, Self.Packet.size());
    ++Self.Installs;
    std::memcpy(Self.Packet.data(), Bytes, Self.Packet.size());
    return S_OK;
  }
  static HRESULT WINAPI getState(WHV_PARTITION_HANDLE Handle, UINT32 Index,
                                 WHV_VIRTUAL_PROCESSOR_STATE_TYPE Type,
                                 VOID *Bytes, UINT32 Size, UINT32 *Written) {
    EXPECT_EQ(Type, WHvVirtualProcessorStateTypeXsaveState);
    return get(Handle, Index, Bytes, Size, Written);
  }
  static HRESULT WINAPI setState(WHV_PARTITION_HANDLE Handle, UINT32 Index,
                                 WHV_VIRTUAL_PROCESSOR_STATE_TYPE Type,
                                 const VOID *Bytes, UINT32 Size) {
    EXPECT_EQ(Type, WHvVirtualProcessorStateTypeXsaveState);
    return set(Handle, Index, Bytes, Size);
  }
};
class WhpXsaveProtocol : public testing::TestWithParam<bool> {
protected:
  WhpAPI API;
  FakeXsave Target;
  WhpXsaveState Transfer;
  X64MachineState State;
  void SetUp() override {
    if (GetParam()) {
      API.WHvGetVirtualProcessorState = FakeXsave::getState;
      API.WHvSetVirtualProcessorState = FakeXsave::setState;
    } else {
      API.WHvGetVirtualProcessorXsaveState = FakeXsave::get;
      API.WHvSetVirtualProcessorXsaveState = FakeXsave::set;
    }
    for (unsigned I = 0; I < State.FP.Registers.size(); ++I)
      State.FP.Registers[I] = Payloads[I];
    State.FP.Tag = UINT8_MAX;
    State.FP.Status = SeedStatus;
    State.FP.Opcode = SeedOpcode;
    State.FP.Instruction = SeedInstruction;
    State.FP.Data = SeedData;
    State.MXCSR = SeedMXCSR;
    for (unsigned I = 0; I < State.Xmm.size(); ++I)
      State.Xmm[I] = {SeedXmm + I, ~SeedXmm - I};
  }
  void initialize() {
    ASSERT_EQ(llvm::toString(Transfer.initialize(API, &Target)), "");
  }
};
TEST_P(WhpXsaveProtocol, CompleteCompactedPacketsRetainEveryPhysicalTOP) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  for (unsigned Top = 0; Top < x64::fp::RegisterCount; ++Top) {
    State.FP.Status = SeedStatus | (Top << x64::fp::TopShift);
    ASSERT_EQ(llvm::toString(Transfer.install(API, &Target, State)), "");
    EXPECT_EQ(llvm::support::endian::read64le(Target.Packet.data() +
                                              x64::fp::XCompOffset),
              x64::fp::Compacted | x64::fp::FPAndSSE);
    X64MachineState Next;
    ASSERT_EQ(llvm::toString(Transfer.capture(API, &Target, Next)), "");
    EXPECT_EQ(Next, State);
  }
  EXPECT_EQ(Target.Queries, 1u);
  EXPECT_EQ(Target.Installs, x64::fp::RegisterCount);
  EXPECT_EQ(Target.Captures, x64::fp::RegisterCount);
}
TEST_P(WhpXsaveProtocol, InvalidQuerySizesCannotAllocateOrInstallState) {
  for (const auto Size :
       {x64::fp::XsaveBytes - 1, x64::fp::MaxXsaveBytes + 1}) {
    Target.Required = Size;
    EXPECT_EQ(llvm::toString(Transfer.initialize(API, &Target)),
              diagnostic::FPState);
  }
  EXPECT_EQ(Target.Installs, 0u);
}
TEST_P(WhpXsaveProtocol, TruncatedAndOversizedCapturesCannotPublishState) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  ASSERT_EQ(llvm::toString(Transfer.install(API, &Target, State)), "");
  for (const auto Written :
       {x64::fp::XsaveBytes - 1, x64::fp::XsaveBytes + 1}) {
    Target.Written = Written;
    auto Next = State;
    EXPECT_EQ(llvm::toString(Transfer.capture(API, &Target, Next)),
              diagnostic::FPState);
    EXPECT_EQ(Next, State);
  }
}
TEST_P(WhpXsaveProtocol, MalformedHostHeaderCannotPublishState) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  ASSERT_EQ(llvm::toString(Transfer.install(API, &Target, State)), "");
  llvm::support::endian::write64le(Target.Packet.data() + x64::fp::XStateOffset,
                                   InvalidPadding);
  auto Next = State;
  EXPECT_EQ(llvm::toString(Transfer.capture(API, &Target, Next)),
            diagnostic::FPState);
  EXPECT_EQ(Next, State);
}
TEST_P(WhpXsaveProtocol, HostFailureRetainsItsStatusAndOriginalState) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  Target.CaptureStatus = E_FAIL;
  auto Next = State;
  EXPECT_EQ(llvm::toString(Transfer.capture(API, &Target, Next)),
            llvm::toString(whpError(diagnostic::WhpState, E_FAIL)));
  EXPECT_EQ(Next, State);
}
TEST_P(WhpXsaveProtocol, DuplicateInitializationKeepsTheOwnedPacket) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  EXPECT_EQ(llvm::toString(Transfer.initialize(API, &Target)),
            diagnostic::WhpState);
  ASSERT_EQ(llvm::toString(Transfer.install(API, &Target, State)), "");
  X64MachineState Next;
  ASSERT_EQ(llvm::toString(Transfer.capture(API, &Target, Next)), "");
  EXPECT_EQ(Next, State);
}
TEST(WhpXsaveAvailability, MissingCompleteStateAPIIsTypedUnavailable) {
  WhpAPI API;
  WhpXsaveState Transfer;
  auto E = Transfer.initialize(API, nullptr);
  EXPECT_TRUE(E.isA<BackendUnavailableError>());
  llvm::consumeError(std::move(E));
}
INSTANTIATE_TEST_SUITE_P(StateAPIs, WhpXsaveProtocol,
                         testing::Values(false, true));
} // namespace
} // namespace neverd::emulation
#endif
