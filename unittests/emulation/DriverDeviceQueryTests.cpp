//===- DriverDeviceQueryTests.cpp - Original device query execution ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute device queries and retained deletion across contracts and backends.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include <filesystem>
#include <map>
#include <tuple>

namespace neverd::emulation {
namespace {
class KernelDeviceQueryExecution
    : public testing::TestWithParam<
          std::tuple<ExecutionBackendKind, ExecutionContract, bool>> {};

void exerciseDeviceQueries(const std::filesystem::path &Image,
                           ExecutionBackendKind Backend,
                           ExecutionContract Contract, bool Rebase) {
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
  for (bool Leak : {false, true}) {
    for (bool Preemptive : {false, true}) {
      DriverOptions Options;
      Options.Backend = Backend;
      Options.Contract = Contract;
      Options.Unload = true;
      Options.ServiceName = Leak ? "DeviceQueriesL" : "DeviceQueries";
      if (Preemptive)
        Options.Scheduling = DriverScheduling{17, 10};
      if (Rebase)
        Options.LoadAddress = 0x1c0000000;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->NTStatus, 0u) << Result->Diagnostic;
      EXPECT_EQ(Result->Stop, Leak ? DriverStopReason::ModelError
                                   : DriverStopReason::Returned)
          << Result->Diagnostic;
      EXPECT_EQ(Result->UnloadCompleted, !Leak);
      EXPECT_TRUE(Result->Devices.empty());
      if (Leak)
        EXPECT_NE(Result->Diagnostic.find("live object references"),
                  std::string::npos);
      std::map<std::string, unsigned> Calls;
      for (const auto &Call : Result->Calls)
        ++Calls[Call.Name];
      for (const auto *Name :
           {"IoGetAttachedDevice", "IoGetAttachedDeviceReference",
            "IoGetDeviceAttachmentBaseRef", "IoGetLowerDeviceObject"})
        EXPECT_GT(Calls[Name], 0u) << Name;
    }
  }
}

TEST_P(KernelDeviceQueryExecution, OriginalDriverRetainsQueriedDeletedDevices) {
  const auto [Backend, Contract, Rebase] = GetParam();
  exerciseDeviceQueries(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                            "driver_device_queries.sys",
                        Backend, Contract, Rebase);
}

TEST_P(KernelDeviceQueryExecution, GenuineWdkRetainsQueriedDeletedDevices) {
#if !defined(NEVERD_WDM_DEVICE_QUERIES_FIXTURE) ||                             \
    !defined(NEVERD_WDM_DEVICE_QUERIES_CFG_FIXTURE)
  GTEST_SKIP() << "genuine WDK device-query fixtures were not supplied";
#else
  const auto [Backend, Contract, Rebase] = GetParam();
  exerciseDeviceQueries(NEVERD_WDM_DEVICE_QUERIES_FIXTURE, Backend, Contract,
                        Rebase);
  if (HasFatalFailure() || IsSkipped())
    return;
  exerciseDeviceQueries(NEVERD_WDM_DEVICE_QUERIES_CFG_FIXTURE, Backend,
                        Contract, Rebase);
#endif
}

std::string deviceQueryParameterName(
    const testing::TestParamInfo<KernelDeviceQueryExecution::ParamType> &Info) {
  const auto [Backend, Contract, Rebase] = Info.param;
  return std::string(executionBackendName(Backend)) +
         (Contract == ExecutionContract::Legacy ? "Legacy" : "Checked") +
         (Rebase ? "Rebased" : "Preferred");
}

INSTANTIATE_TEST_SUITE_P(
    Backends, KernelDeviceQueryExecution,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64),
                     testing::Bool()),
    deviceQueryParameterName);
} // namespace
} // namespace neverd::emulation
