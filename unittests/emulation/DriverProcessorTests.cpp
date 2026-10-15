//===- DriverProcessorTests.cpp - Processor query and IRQL execution -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify original import and WDK dynamic calls in entry, worker and DPC
/// frames.

#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include <algorithm>
#include <filesystem>
#include <tuple>

namespace neverd::emulation {
namespace {
class KernelProcessorExecution
    : public testing::TestWithParam<
          std::tuple<ExecutionBackendKind, ExecutionContract, bool>> {
protected:
  void run(const std::filesystem::path &Image) {
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
    Probe->CPU.reset();
    for (bool Scheduled : {false, true})
      for (char Mode : {'S', 'H'}) {
        SCOPED_TRACE(::testing::Message()
                     << Image << ' ' << Scheduled << ' ' << Mode);
        DriverOptions Options;
        Options.Backend = Backend;
        Options.Contract = Contract;
        Options.Unload = true;
        Options.ServiceName = std::string("ProcessorQueries") + Mode;
        if (Rebase)
          Options.LoadAddress = 0x1c0000000;
        if (Scheduled)
          Options.Scheduling = DriverScheduling{17, 10};
        auto Result = emulateDriver(Image, Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        if (Mode == 'H') {
          EXPECT_EQ(Result->Stop, DriverStopReason::ModelError)
              << Result->Diagnostic;
          EXPECT_NE(Result->Diagnostic.find("KeRaiseIrqlToDpcLevel"),
                    std::string::npos);
          EXPECT_FALSE(Result->UnloadCompleted);
          continue;
        }
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        EXPECT_TRUE(Result->UnloadCompleted) << Result->Diagnostic;
        EXPECT_EQ(Result->NTStatus, 0u);
        EXPECT_TRUE(Result->Devices.empty());
        EXPECT_GE(std::count_if(Result->Calls.begin(), Result->Calls.end(),
                                [](const auto &Call) {
                                  return Call.Name ==
                                         "KeGetCurrentProcessorNumberEx";
                                }),
                  15);
        EXPECT_GE(std::count_if(Result->Calls.begin(), Result->Calls.end(),
                                [](const auto &Call) {
                                  return Call.Name == "KeRaiseIrqlToDpcLevel";
                                }),
                  3);
      }
  }
};

TEST_P(KernelProcessorExecution, OriginalImportsAndLookupShareProcessorState) {
  run(std::filesystem::path(NEVERD_DRIVER_FIXTURES) / "driver_processors.sys");
}
TEST_P(KernelProcessorExecution,
       GenuineWdkInlineAndExportCallsShareProcessorState) {
#if !defined(NEVERD_WDM_PROCESSORS_FIXTURE) ||                                 \
    !defined(NEVERD_WDM_PROCESSORS_CFG_FIXTURE)
  GTEST_SKIP() << "genuine WDK processor fixture was not supplied";
#else
  run(NEVERD_WDM_PROCESSORS_FIXTURE);
  if (HasFatalFailure() || IsSkipped())
    return;
#ifdef NEVERD_WDM_PROCESSORS_CFG_FIXTURE
  run(NEVERD_WDM_PROCESSORS_CFG_FIXTURE);
#endif
#endif
}

std::string processorParameterName(
    const testing::TestParamInfo<KernelProcessorExecution::ParamType> &Info) {
  const auto [Backend, Contract, Rebase] = Info.param;
  return std::string(executionBackendName(Backend)) +
         (Contract == ExecutionContract::Legacy ? "Legacy" : "Checked") +
         (Rebase ? "Rebased" : "Preferred");
}

INSTANTIATE_TEST_SUITE_P(
    Backends, KernelProcessorExecution,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64),
                     testing::Bool()),
    processorParameterName);
} // namespace
} // namespace neverd::emulation
