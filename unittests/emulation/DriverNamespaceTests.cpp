//===- DriverNamespaceTests.cpp - Original namespace execution -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Execute original named-object calls across contracts and backends.

#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include <filesystem>
#include <map>
#include <tuple>

namespace neverd::emulation {
namespace {

class KernelNamespaceExecution
    : public testing::TestWithParam<
          std::tuple<ExecutionBackendKind, ExecutionContract, bool>> {};

TEST_P(KernelNamespaceExecution,
       OriginalDriverPreservesNamespaceAndHeldObjectLifetimes) {
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
  for (bool Preemptive : {false, true}) {
    DriverOptions Options;
    Options.Backend = Backend;
    Options.Contract = Contract;
    Options.Unload = true;
    if (Preemptive)
      Options.Scheduling = DriverScheduling{17, 10};
    if (Rebase)
      Options.LoadAddress = 0x1c0000000;
    auto Result = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                    "driver_namespace.sys",
                                Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    EXPECT_EQ(Result->NTStatus, 0u) << "preemptive=" << Preemptive;
    EXPECT_TRUE(Result->UnloadCompleted);
    std::map<std::string, unsigned> Calls;
    for (const auto &Call : Result->Calls)
      ++Calls[Call.Name];
#define NEVERD_KERNEL_OBJECT_API(Name, Arity, IRQL, Operation)                 \
  EXPECT_GT(Calls[#Name], 0u) << #Name;
#include "os/windows/kernel/KernelNamespaceAPIs.def"
#undef NEVERD_KERNEL_OBJECT_API
    EXPECT_EQ(Calls["IoCreateDevice"], 1u);
    EXPECT_EQ(Calls["IoDeleteSymbolicLink"], 2u);
    EXPECT_EQ(Calls["ZwClose"], 3u);
  }
}

std::string namespaceParameterName(
    const testing::TestParamInfo<KernelNamespaceExecution::ParamType> &Info) {
  const auto [Backend, Contract, Rebase] = Info.param;
  return std::string(executionBackendName(Backend)) +
         (Contract == ExecutionContract::Legacy ? "Legacy" : "Checked") +
         (Rebase ? "Rebased" : "Preferred");
}

INSTANTIATE_TEST_SUITE_P(
    Backends, KernelNamespaceExecution,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64),
                     testing::Bool()),
    namespaceParameterName);

} // namespace
} // namespace neverd::emulation
