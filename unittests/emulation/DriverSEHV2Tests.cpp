//===- DriverSEHV2Tests.cpp - Linked V2 helper exception execution --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include <filesystem>

namespace neverd::emulation {
namespace {
class DriverSEHV2Execution
    : public testing::TestWithParam<ExecutionBackendKind> {};

TEST_P(DriverSEHV2Execution, OriginalHelpersRaiseCatchAndContinue) {
  DriverOptions Options;
  Options.Backend = GetParam();
  Options.Contract = ExecutionContract::CheckedX64;
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
  for (uint64_t Base : {0ULL, 0x1c0000000ULL}) {
    SCOPED_TRACE(Base);
    Options.LoadAddress = Base;
    auto Run = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_seh_v2.sys",
                             Options);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
    EXPECT_EQ(Run->NTStatus, 0u);
    size_t Raises = 0;
    for (const auto &Call : Run->Calls)
      if (Call.Name == "ExRaiseStatus") {
        ++Raises;
        EXPECT_FALSE(Call.Result.has_value());
      }
    EXPECT_EQ(Raises, 3u);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Native, DriverSEHV2Execution,
    testing::Values(ExecutionBackendKind::Unicorn, ExecutionBackendKind::KVM,
                    ExecutionBackendKind::WHP, ExecutionBackendKind::HVF),
    [](const testing::TestParamInfo<ExecutionBackendKind> &Info) {
      return std::string(executionBackendName(Info.param));
    });
} // namespace
} // namespace neverd::emulation
