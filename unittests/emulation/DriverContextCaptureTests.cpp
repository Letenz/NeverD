//===- DriverContextCaptureTests.cpp - Live caller capture integration ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent compiled caller-state oracle over every checked transport.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/windows/driver/DriverContextCapture.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"
#include "neverd/emulation/ProcessObserver.h"
#include "neverd/emulation/ProcessRuntimeState.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
class DriverContextCapture
    : public testing::TestWithParam<ExecutionBackendKind> {
protected:
  DriverOptions Options;
  std::filesystem::path Path = std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                               "driver_context_capture.sys";
  void SetUp() override {
    Options.Backend = GetParam();
    Options.Contract = ExecutionContract::CheckedX64;
    // Twelve complete guarded records, each with independent bytewise oracles.
    Options.InstructionLimit = 400000;
    auto Probe = createExecutionBackend(Options.Backend, Options.Contract,
                                        Options.MemoryLimit);
    if (!Probe) {
      auto E = Probe.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Text = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
    ASSERT_TRUE(std::filesystem::is_regular_file(Path));
    Options.Unload = true;
    for (auto Kind :
         {DriverRequestKind::Create, DriverRequestKind::DeviceControl,
          DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
      DriverRequest Request;
      Request.Kind = Kind;
      if (Kind == DriverRequestKind::DeviceControl) {
        Request.ControlCode = 0x222000;
        Request.OutputSize = 4;
      }
      Options.Requests.push_back(Request);
    }
  }
};

TEST_P(DriverContextCapture, CollectorUsesLiveRegistersAndLogicalSelectors) {
  auto Backend = createExecutionBackend(Options.Backend, Options.Contract,
                                        Options.MemoryLimit);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  auto &CPU = *Backend->CPU;
  const CPURegister Registers[] = {
      CPURegister::X64AX,  CPURegister::X64CX,  CPURegister::X64DX,
      CPURegister::X64BX,  CPURegister::X64SP,  CPURegister::X64BP,
      CPURegister::X64SI,  CPURegister::X64DI,  CPURegister::X64R8,
      CPURegister::X64R9,  CPURegister::X64R10, CPURegister::X64R11,
      CPURegister::X64R12, CPURegister::X64R13, CPURegister::X64R14,
      CPURegister::X64R15};
  auto Set = [&](CPURegister R, uint64_t Low, uint64_t High = 0) {
    auto E = CPU.writeRegister(R, {Low, High});
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  };
  using namespace llvm::support::endian;
  for (uint16_t Control : {0x27fu, 0x37fu}) {
    std::array<uint8_t, 1232> Expected{};
    for (unsigned I = 0; I < 16; ++I) {
      Set(Registers[I], 0x12340000 + I);
      write64le(Expected.data() + 120 + I * 8, 0x12340000 + I);
      Set(vectorRegister(GuestArchitecture::X64, I), 0x1122334455667700ULL + I,
          0x8877665544332200ULL + I);
      write64le(Expected.data() + 416 + I * 16, 0x1122334455667700ULL + I);
      write64le(Expected.data() + 424 + I * 16, 0x8877665544332200ULL + I);
    }
    Set(CPURegister::X64PC, 0x180001234);
    Set(CPURegister::X64FLAGS, 0x6d7);
    Set(CPURegister::X64FPCW, Control);
    Set(CPURegister::X64MXCSR, 0x3f80);
    write32le(Expected.data() + 52, 0x3f80);
    for (unsigned I = 0; I < 6; ++I)
      write16le(Expected.data() + 56 + I * 2, I ? 16 : 8);
    write32le(Expected.data() + 68, 0x6d7);
    write64le(Expected.data() + 248, 0x180001234);
    write16le(Expected.data() + 256, Control);
    write32le(Expected.data() + 280, 0x3f80);
    auto Captured = collectDriverContext(CPU);
    ASSERT_TRUE(bool(Captured)) << llvm::toString(Captured.takeError());
    EXPECT_EQ(Captured->Bytes, Expected);
  }
}

TEST_P(DriverContextCapture, CompleteImportedAndDynamicRecordsAcrossLifecycle) {
  for (uint64_t Base : {uint64_t(0), uint64_t(0x1a0000000)}) {
    Options.LoadAddress = Base;
    auto Run = emulateDriver(Path, Options);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << driverResultJSON(*Run);
    EXPECT_EQ(Run->NTStatus, 0u);
    EXPECT_TRUE(Run->UnloadCompleted);
    EXPECT_TRUE(Run->Devices.empty());
    ASSERT_EQ(Run->Requests.size(), 4u);
    for (const auto &Request : Run->Requests) {
      EXPECT_TRUE(Request.Completed);
      EXPECT_EQ(Request.IOStatus, 0u);
    }
    ASSERT_EQ(Run->Requests[1].Output.size(), 4u);
    EXPECT_EQ(llvm::support::endian::read32le(Run->Requests[1].Output.data()),
              0x43545831u);
    EXPECT_NE(std::find(Run->Messages.begin(), Run->Messages.end(),
                        "caller context unload complete"),
              Run->Messages.end());
  }
}

TEST_P(DriverContextCapture, InvalidDestinationsAndLegacyFailClearly) {
  for (uint32_t Code : {0x222004u, 0x222008u}) {
    Options.Requests[1].ControlCode = Code;
    auto Run = emulateDriver(Path, Options);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, DriverStopReason::ModelError) << Run->Diagnostic;
    EXPECT_NE(Run->Diagnostic.find(Code == 0x222004 ? "invalid buffer extent"
                                                    : "16-byte aligned"),
              std::string::npos)
        << Run->Diagnostic;
    EXPECT_FALSE(Run->UnloadCompleted);
  }
  // Native transport admission rejects legacy before executing this fixture.
  // The software legacy path still must reject this API at its shared owner.
  if (GetParam() == ExecutionBackendKind::Unicorn) {
    Options.Contract = ExecutionContract::Legacy;
    auto Run = emulateDriver(Path, Options);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, DriverStopReason::ModelError) << Run->Diagnostic;
    EXPECT_NE(Run->Diagnostic.find("requires checked-x64"), std::string::npos);
    EXPECT_TRUE(Run->Requests.empty());
  }
}

class CaptureDependency final : public ProcessObserver {
public:
  unsigned Captures = 0, Returns = 0;
  bool Pending = false;
  llvm::Expected<std::vector<ExecutionWatch>> started(ProcessView &) override {
    return std::vector<ExecutionWatch>{};
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  watched(ProcessView &, uint64_t) override {
    return std::vector<ExecutionWatch>{};
  }
  llvm::Error exporting(ProcessView &P, const ProcessExportView &Export,
                        std::optional<uint64_t>) override {
    if (Export.Name != "RtlCaptureContext")
      return llvm::Error::success();
    auto State = P.runtimeState();
    if (!State)
      return State.takeError();
    if (!*State)
      return llvm::createStringError("missing driver runtime state");
    // The in-progress stack already differs from DriverEntry's baseline.
    // Only the environment-effects dependency is introduced by this API.
    if (!Captures) {
      const auto Reasons = (*State)->additionalDependencyReasons();
      EXPECT_EQ(std::find(Reasons.begin(), Reasons.end(),
                          "unreconstructed kernel or environment effects"),
                Reasons.end());
    }
    ++Captures;
    Pending = true;
    return llvm::Error::success();
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  resuming(ProcessView &P) override {
    if (!Pending)
      return std::nullopt;
    auto State = P.runtimeState();
    if (!State)
      return State.takeError();
    if (!*State)
      return llvm::createStringError("missing driver runtime state");
    EXPECT_TRUE((*State)->hasAdditionalDependencies());
    const auto Reasons = (*State)->additionalDependencyReasons();
    EXPECT_NE(std::find(Reasons.begin(), Reasons.end(),
                        "unreconstructed kernel or environment effects"),
              Reasons.end());
    ++Returns;
    Pending = false;
    return std::nullopt;
  }
};

TEST_P(DriverContextCapture, ConcreteCaptureRetainsRecoveryDependency) {
  CaptureDependency Observer;
  auto Run = observeDriver(Path, Options, Observer);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << driverResultJSON(*Run);
  EXPECT_EQ(Run->NTStatus, 0u);
  EXPECT_TRUE(Run->UnloadCompleted);
  EXPECT_EQ(Observer.Captures, 12u);
  EXPECT_EQ(Observer.Returns, 12u);
  EXPECT_FALSE(Observer.Pending);
}

INSTANTIATE_TEST_SUITE_P(
    Native, DriverContextCapture,
    testing::Values(ExecutionBackendKind::Unicorn, ExecutionBackendKind::KVM,
                    ExecutionBackendKind::WHP),
    [](const testing::TestParamInfo<ExecutionBackendKind> &P) {
      return std::string(executionBackendName(P.param));
    });
} // namespace
} // namespace neverd::emulation
