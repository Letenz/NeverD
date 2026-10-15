//===- DriverDataExportTests.cpp - Imported data execution contracts -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execute original data-import fixtures across driver CPU contracts.
///
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include <filesystem>
#include <tuple>

namespace neverd::emulation {
namespace {
class KernelDataExecution
    : public testing::TestWithParam<
          std::tuple<ExecutionBackendKind, ExecutionContract, bool>> {
protected:
  void SetUp() override {
    const auto [Backend, Contract, Rebase] = GetParam();
    auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
    if (!Probe) {
      auto E = Probe.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
  }
  DriverOptions options(char Action) const {
    const auto [Backend, Contract, Rebase] = GetParam();
    DriverOptions Options;
    Options.Backend = Backend;
    Options.Contract = Contract;
    Options.ServiceName = std::string("NeverDData") + Action;
    Options.Unload = true;
    if (Rebase)
      Options.LoadAddress = 0x1c0000000;
    return Options;
  }
  llvm::Expected<DriverResult> run(const DriverOptions &Options) {
    return emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                             "driver_data.sys",
                         Options);
  }
};

TEST_P(KernelDataExecution,
       OriginalDataImportsShareIdentityAcrossLookupAndUnload) {
  auto Options = options('0');
  Options.Scheduling = DriverScheduling{17, 10};
  auto Result = run(Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_EQ(Result->NTStatus, 0u);
  EXPECT_TRUE(Result->UnloadCompleted);
  unsigned Lookups = 0;
  for (const auto &Call : Result->Calls)
    if (Call.Name == "MmGetSystemRoutineAddress")
      ++Lookups;
  EXPECT_EQ(Lookups, 12u);
}

TEST_P(KernelDataExecution, DeclaredDebuggerBytesMatchDirectAndAPIReads) {
  for (char Action : {'1', '4'})
    for (bool Enabled : {false, true})
      for (bool NotPresent : {false, true}) {
        auto Options = options(Action);
        Options.DebuggerState = DriverDebuggerState{Enabled, NotPresent};
        auto Result = run(Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        EXPECT_EQ(Result->NTStatus, 0u);
        EXPECT_EQ(Result->Messages,
                  std::vector<std::string>{
                      "debugger bytes: " + std::to_string(Enabled) + "," +
                      std::to_string(NotPresent)});
        EXPECT_TRUE(Result->UnloadCompleted);
      }
}

TEST_P(KernelDataExecution, UndeclaredReadsWritesAndExecutionFail) {
  for (char Action : {'1', '2', '3', '4'}) {
    SCOPED_TRACE(Action);
    auto Result = run(options(Action));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_TRUE(
        Result->Stop == DriverStopReason::ModelError ||
        Result->Stop == DriverStopReason::MemoryFault ||
        (Action == '3' && Result->Stop == DriverStopReason::UnsupportedAPI))
        << driverStopReasonName(Result->Stop) << ": " << Result->Diagnostic;
    EXPECT_FALSE(Result->UnloadCompleted);
  }
}

std::string parameterName(
    const testing::TestParamInfo<KernelDataExecution::ParamType> &Info) {
  const auto [Backend, Contract, Rebase] = Info.param;
  return std::string(executionBackendName(Backend)) +
         (Contract == ExecutionContract::Legacy ? "Legacy" : "Checked") +
         (Rebase ? "Rebased" : "Preferred");
}
INSTANTIATE_TEST_SUITE_P(
    Backends, KernelDataExecution,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64),
                     testing::Bool()),
    parameterName);
} // namespace
} // namespace neverd::emulation
