//===- DriverLoaderViewTests.cpp - Loader view instruction execution ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute original loader accesses across available x64 CPU backends.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"
#include "neverd/emulation/ProcessObserver.h"
#include "neverd/emulation/ProcessRuntimeState.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <filesystem>
#include <tuple>

namespace neverd::emulation {
namespace {
using Parameter = std::tuple<ExecutionBackendKind, ExecutionContract>;

class LoaderObserver final : public ProcessObserver {
public:
  std::shared_ptr<const ProcessRuntimeState> Initial;
  llvm::Expected<std::vector<ExecutionWatch>> started(ProcessView &P) override {
    auto State = P.runtimeState(true);
    if (!State)
      return State.takeError();
    Initial = std::move(*State);
    return std::vector<ExecutionWatch>{};
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  watched(ProcessView &, uint64_t) override {
    return std::nullopt;
  }
};

class DriverLoaderViewExecution : public testing::TestWithParam<Parameter> {
protected:
  DriverOptions Options;
  std::filesystem::path Path =
      std::filesystem::path(NEVERD_DRIVER_FIXTURES) / "driver_loader_view.sys";
  void SetUp() override {
    std::tie(Options.Backend, Options.Contract) = GetParam();
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

TEST_P(DriverLoaderViewExecution, DeclaredFieldsSurviveRebasedLifecycle) {
  for (auto Base : {uint64_t(0), uint64_t(0x1a0000000)})
    for (uint32_t Flags : {0u, UINT32_MAX}) {
      SCOPED_TRACE(Base);
      SCOPED_TRACE(Flags);
      Options.LoadAddress = Base;
      Options.LoaderView =
          DriverLoaderView{1, Flags, "original lifecycle fixture"};
      LoaderObserver Observer;
      auto Run = observeDriver(Path, Options, Observer);
      ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
      ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
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
                Flags);
      EXPECT_NE(std::find(Run->Messages.begin(), Run->Messages.end(),
                          "loader view unload complete"),
                Run->Messages.end());
      ASSERT_TRUE(Observer.Initial);
      EXPECT_TRUE(Observer.Initial->hasAdditionalDependencies());
      const auto Reasons = Observer.Initial->additionalDependencyReasons();
      EXPECT_NE(std::find(Reasons.begin(), Reasons.end(),
                          "unreconstructed kernel or environment effects"),
                Reasons.end());
    }
}

TEST_P(DriverLoaderViewExecution, DefaultAndForbiddenFootprintsStop) {
  auto Default = emulateDriver(Path, Options);
  ASSERT_TRUE(bool(Default)) << llvm::toString(Default.takeError());
  EXPECT_EQ(Default->Stop, DriverStopReason::ModelError) << Default->Diagnostic;
  EXPECT_NE(Default->Diagnostic.find("unmodeled"), std::string::npos);
  EXPECT_TRUE(Default->Requests.empty());
  Options.LoaderView = DriverLoaderView{1, UINT32_MAX, "refusal controls"};
  for (uint32_t Code : {0x222004u, 0x222008u, 0x22200cu}) {
    SCOPED_TRACE(Code);
    Options.Requests[1].ControlCode = Code;
    auto Run = emulateDriver(Path, Options);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, DriverStopReason::ModelError) << Run->Diagnostic;
    EXPECT_NE(
        Run->Diagnostic.find(Code == 0x222008 ? "read-only" : "unmodeled"),
        std::string::npos);
    EXPECT_FALSE(Run->UnloadCompleted);
  }
}

TEST_P(DriverLoaderViewExecution, RejectedCPUStorePreservesTheWholeRecord) {
  auto Backend = createExecutionBackend(Options.Backend, Options.Contract,
                                        Options.MemoryLimit);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  auto &CPU = *Backend->CPU;
  constexpr uint64_t Code = 0x180001000;
  llvm::cantFail(CPU.map(Code, 4096, Read | Write | Execute));
  DriverImage Image;
  Image.Base = Code - 4096;
  Image.Entry = Code;
  Image.Size = 8192;
  DriverResult Result;
  KernelModel Model(CPU, Result);
  Options.LoaderView = DriverLoaderView{1, UINT32_MAX, "failed CPU store"};
  llvm::cantFail(Model.initialize(Image, Options));
  const auto Record =
      llvm::cantFail(CPU.readInteger(Model.driverObject() + 40, 8));
  std::array<uint8_t, 160> Before{}, After{};
  llvm::cantFail(CPU.snapshotBacking(Record, Before));
  // mov dword ptr [rcx+104],0; ud2. The rejected store must not commit.
  const uint8_t Body[] = {0xc7, 0x41, 0x68, 0, 0, 0, 0, 0x0f, 0x0b};
  llvm::cantFail(CPU.write(Code, Body));
  llvm::cantFail(CPU.setReg(X64Register::CX, Record));
  std::string Diagnostic;
  BackendHooks Hooks;
  Hooks.Write = [&](uint64_t Address, uint32_t Size, uint64_t) {
    if (auto E = Model.validateGuestAccess(Address, Size, true)) {
      Diagnostic = llvm::toString(std::move(E));
      CPU.stop();
    }
  };
  llvm::cantFail(CPU.installHooks(std::move(Hooks)));
  auto Exit = CPU.runUntilExit(Code, 1000000);
  ASSERT_TRUE(bool(Exit)) << llvm::toString(Exit.takeError());
  EXPECT_NE(Diagnostic.find("read-only"), std::string::npos);
  llvm::cantFail(CPU.snapshotBacking(Record, After));
  EXPECT_EQ(After, Before);
}

TEST(DriverLoaderViewDeclaration, LegacyFailsBeforeLoadOrObservation) {
  for (auto Kind : {ExecutionBackendKind::Auto, ExecutionBackendKind::Unicorn,
                    ExecutionBackendKind::KVM, ExecutionBackendKind::WHP}) {
    DriverOptions Options;
    Options.Backend = Kind;
    Options.LoaderView = DriverLoaderView{1, 0, "legacy refusal"};
    LoaderObserver Observer;
    auto Result = observeDriver("absent.sys", Options, Observer);
    ASSERT_FALSE(bool(Result));
    EXPECT_EQ(llvm::toString(Result.takeError()),
              "driver loader_view requires checked-x64 execution");
    EXPECT_FALSE(Observer.Initial);
  }
}

std::string parameterName(const testing::TestParamInfo<Parameter> &P) {
  const auto &[Backend, Contract] = P.param;
  return std::string(executionBackendName(Backend)) +
         (Contract == ExecutionContract::Legacy ? "_driver" : "_checked");
}
INSTANTIATE_TEST_SUITE_P(
    Native, DriverLoaderViewExecution,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP),
                     testing::Values(ExecutionContract::CheckedX64)),
    parameterName);
} // namespace
} // namespace neverd::emulation
