//===- DriverFileObjectTests.cpp - Retained file execution ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute deferred file CLOSE callbacks across contracts and backends.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include <filesystem>
#include <map>
#include <tuple>

namespace neverd::emulation {
namespace {
class KernelFileObjectExecution
    : public testing::TestWithParam<
          std::tuple<ExecutionBackendKind, ExecutionContract, bool>> {};

TEST_P(KernelFileObjectExecution, OriginalDriverDefersCloseUntilLastReference) {
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
  for (char Mode : {'S', 'P', 'W', 'F', 'L'}) {
    for (bool Preemptive : {false, true}) {
      SCOPED_TRACE(std::string(1, Mode) +
                   (Preemptive ? " scheduled" : " cooperative"));
      DriverOptions Options;
      Options.Backend = Backend;
      Options.Contract = Contract;
      Options.Unload = true;
      Options.ServiceName = std::string("FileObjects") + Mode;
      if (Preemptive)
        Options.Scheduling = DriverScheduling{17, 10};
      if (Rebase)
        Options.LoadAddress = 0x1c0000000;
      auto Add = [&](DriverRequestKind Kind, uint32_t PID) {
        DriverRequest Request;
        Request.Kind = Kind;
        Request.File = 1;
        Request.RequestorProcessID = PID;
        Options.Requests.push_back(std::move(Request));
      };
      Add(DriverRequestKind::Create, 0x1000);
      if (Mode != 'F') {
        Add(DriverRequestKind::Cleanup, 0x1000);
        Add(DriverRequestKind::Close, 0x1000);
      }
      Add(DriverRequestKind::Create, 0x2000);
      Add(DriverRequestKind::DeviceControl, 0x2000);
      Add(DriverRequestKind::Cleanup, 0x2000);
      Add(DriverRequestKind::Close, 0x2000);
      auto Result =
          emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                            "driver_file_objects.sys",
                        Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->NTStatus, 0u) << Result->Diagnostic;
      EXPECT_EQ(Result->Stop, Mode == 'L' ? DriverStopReason::ModelError
                                          : DriverStopReason::Returned)
          << Result->Diagnostic;
      EXPECT_EQ(Result->UnloadCompleted, Mode != 'L');
      ASSERT_EQ(Result->Requests.size(), Options.Requests.size());
      for (size_t I = 0; I < Result->Requests.size(); ++I) {
        const auto &Request = Result->Requests[I];
        if (Mode == 'L' && I == 2) {
          EXPECT_EQ(Request.IRP, 0u);
          EXPECT_FALSE(Request.Completed);
          EXPECT_FALSE(Request.DispatchStatus);
          continue;
        }
        EXPECT_TRUE(Request.Completed) << I;
        EXPECT_EQ(Request.IOStatus, Mode == 'F' && !I ? 0xc0000001u : 0u) << I;
      }
      if (Mode == 'P') {
        EXPECT_EQ(Result->Requests[2].DispatchStatus, 0x103u);
        EXPECT_EQ(Result->Requests[2].RequestorProcessID, 0x2000u);
      }
      if (Mode == 'W')
        EXPECT_EQ(Result->Requests[2].RequestorProcessID, 4u);
      if (Mode == 'L')
        EXPECT_NE(Result->Diagnostic.find("file closed"), std::string::npos);
      else
        EXPECT_TRUE(Result->Devices.empty());
    }
  }
}

std::string fileObjectParameterName(
    const testing::TestParamInfo<KernelFileObjectExecution::ParamType> &Info) {
  const auto [Backend, Contract, Rebase] = Info.param;
  return std::string(executionBackendName(Backend)) +
         (Contract == ExecutionContract::Legacy ? "Legacy" : "Checked") +
         (Rebase ? "Rebased" : "Preferred");
}

INSTANTIATE_TEST_SUITE_P(
    Backends, KernelFileObjectExecution,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64),
                     testing::Bool()),
    fileObjectParameterName);
} // namespace
} // namespace neverd::emulation
