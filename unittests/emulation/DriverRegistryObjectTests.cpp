//===- DriverRegistryObjectTests.cpp - Original registry objects -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Execute original registry-object calls across contracts and backends.

#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include <filesystem>
#include <map>
#include <tuple>

namespace neverd::emulation {
namespace {

class KernelRegistryObjectExecution
    : public testing::TestWithParam<
          std::tuple<ExecutionBackendKind, ExecutionContract, bool>> {};

TEST_P(KernelRegistryObjectExecution,
       OriginalDriverSharesKeyHandlesAndDetectsPointerLeaks) {
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
  for (bool Leak : {false, true}) {
    for (bool Preemptive : {false, true}) {
      DriverOptions Options;
      Options.Backend = Backend;
      Options.Contract = Contract;
      Options.Unload = true;
      Options.ServiceName = Leak ? "RegistryObjectsL" : "RegistryObjects";
      Options.Registry = std::vector<DriverRegistryKey>{
          {"\\Registry\\Machine\\Software\\OriginalObjects", {}}};
      if (Preemptive)
        Options.Scheduling = DriverScheduling{17, 10};
      if (Rebase)
        Options.LoadAddress = 0x1c0000000;
      auto Result =
          emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                            "driver_registry_objects.sys",
                        Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->NTStatus, 0u) << Result->Diagnostic;
      EXPECT_EQ(Result->Stop, Leak ? DriverStopReason::ModelError
                                   : DriverStopReason::Returned)
          << Result->Diagnostic;
      EXPECT_EQ(Result->UnloadCompleted, !Leak);
      if (Leak)
        EXPECT_NE(Result->Diagnostic.find("live object references"),
                  std::string::npos);
      std::map<std::string, unsigned> Calls;
      for (const auto &Call : Result->Calls)
        ++Calls[Call.Name];
      for (const auto *Name :
           {"ZwOpenKey", "ZwCreateKey", "ZwDeleteKey", "ZwQueryKey",
            "ZwQueryObject", "ZwClose", "ObReferenceObjectByHandle",
            "ObReferenceObjectByPointer", "ObOpenObjectByPointer",
            "ObQueryNameString", "ObGetObjectType", "ObfDereferenceObject"})
        EXPECT_GT(Calls[Name], 0u) << Name;
    }
  }
}

std::string registryObjectParameterName(
    const testing::TestParamInfo<KernelRegistryObjectExecution::ParamType>
        &Info) {
  const auto [Backend, Contract, Rebase] = Info.param;
  return std::string(executionBackendName(Backend)) +
         (Contract == ExecutionContract::Legacy ? "Legacy" : "Checked") +
         (Rebase ? "Rebased" : "Preferred");
}

INSTANTIATE_TEST_SUITE_P(
    Backends, KernelRegistryObjectExecution,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64),
                     testing::Bool()),
    registryObjectParameterName);

} // namespace
} // namespace neverd::emulation
