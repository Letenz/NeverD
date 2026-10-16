//===- DriverRuntimeStringTests.cpp - Checked CRT execution --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise original and genuine WDK CRT calls in entry, worker and unload.

#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include <filesystem>
#include <map>
#include <tuple>

namespace neverd::emulation {
namespace {
class KernelCRTStringExecution
    : public testing::TestWithParam<
          std::tuple<ExecutionBackendKind, ExecutionContract, bool>> {
protected:
  void run(const std::filesystem::path &Image, bool SDKLinkedCopy = false) {
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
        Options.ServiceName = std::string("CheckedCRT") + Mode;
        if (Rebase)
          Options.LoadAddress = 0x1c0000000;
        if (Scheduled)
          Options.Scheduling = DriverScheduling{17, 10};
        auto Result = emulateDriver(Image, Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        EXPECT_TRUE(Result->UnloadCompleted) << Result->Diagnostic;
        EXPECT_EQ(Result->NTStatus, 0u);
        EXPECT_TRUE(Result->Devices.empty());
        std::map<std::string, unsigned> Calls;
        for (const auto &Call : Result->Calls)
          ++Calls[Call.Name];
        for (auto [Name, Count] : {std::pair{"isdigit", 36u},
                                   {"_strupr", 6u},
                                   {"_wcslwr", 6u},
                                   {"_wcsupr", 6u},
                                   {"strcpy_s", 6u},
                                   {"strncpy_s", 18u},
                                   {"wcscat_s", 12u},
                                   {"wcsncpy_s", 12u},
                                   {"memcpy_s", SDKLinkedCopy ? 9u : 18u},
                                   {"memmove_s", 18u},
                                   {"RtlImageNtHeader", 12u},
                                   {"RtlHashUnicodeString", 18u},
                                   {"RtlUpcaseUnicodeString", 18u},
                                   {"RtlTimeToSecondsSince1980", 24u},
                                   {"RtlFreeUnicodeString", 6u}})
          EXPECT_EQ(Calls[Name], Count) << Name;
        for (const char *Name : {"toupper", "towupper", "RtlUpcaseUnicodeChar"})
          EXPECT_EQ(Calls[Name], 12u) << Name;
        EXPECT_EQ(Calls["MmGetSystemRoutineAddress"], 24u);
        EXPECT_EQ(Calls["ObGetFilterVersion"], 6u);
        EXPECT_EQ(Calls["IoGetInitialStack"], 6u);
        EXPECT_EQ(Calls["IoGetStackLimits"], 12u);
        EXPECT_EQ(Calls["IoThreadToProcess"], 6u);
        EXPECT_EQ(Calls["PsGetCurrentThreadPreviousMode"], 6u);
        EXPECT_EQ(Calls["RtlPcToFileHeader"], 12u);
        EXPECT_EQ(Calls["ExGetPreviousMode"], 6u);
        EXPECT_EQ(Calls["vDbgPrintExWithPrefix"], 24u);
        ASSERT_EQ(Result->Messages.size(), 24u);
        for (unsigned I = 0; I < 24; I += 4) {
          EXPECT_EQ(Result->Messages[I], "crt:384|1122334455667788|abc");
          EXPECT_EQ(Result->Messages[I + 1], std::string(127, 'P'));
          EXPECT_EQ(Result->Messages[I + 2],
                    "crt:" + std::string(506, '0') + "\n");
          EXPECT_EQ(Result->Messages[I + 3], "crt:A");
        }
      }
  }
};

TEST_P(KernelCRTStringExecution,
       OriginalImportsAndLookupPreserveCheckedWrites) {
  run(std::filesystem::path(NEVERD_DRIVER_FIXTURES) / "driver_crt_strings.sys");
}

TEST_P(KernelCRTStringExecution, GenuineWdkCallsPreserveCheckedWrites) {
#if !defined(NEVERD_WDM_CRT_STRINGS_FIXTURE) ||                                \
    !defined(NEVERD_WDM_CRT_STRINGS_CFG_FIXTURE)
  GTEST_SKIP() << "genuine WDK CRT fixtures were not supplied";
#else
  // The pinned WDK's ntoskrnl.lib provides memcpy_s as linked guest code;
  // only dynamically resolved copies cross its kernel export gateway.
  run(NEVERD_WDM_CRT_STRINGS_FIXTURE, true);
  if (HasFatalFailure() || IsSkipped())
    return;
  run(NEVERD_WDM_CRT_STRINGS_CFG_FIXTURE, true);
#endif
}

std::string crtParameterName(
    const testing::TestParamInfo<KernelCRTStringExecution::ParamType> &Info) {
  const auto [Backend, Contract, Rebase] = Info.param;
  return std::string(executionBackendName(Backend)) +
         (Contract == ExecutionContract::Legacy ? "Legacy" : "Checked") +
         (Rebase ? "Rebased" : "Preferred");
}

INSTANTIATE_TEST_SUITE_P(
    Backends, KernelCRTStringExecution,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64),
                     testing::Bool()),
    crtParameterName);
} // namespace
} // namespace neverd::emulation
