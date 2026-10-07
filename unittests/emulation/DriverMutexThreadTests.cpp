//===- DriverMutexThreadTests.cpp - Original thread-owned mutex paths -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "fixtures/driver_seh_test.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include <algorithm>
#include <ostream>
#include <string>
#include <vector>

namespace neverd::emulation {
namespace {
struct MutexParameter {
  ExecutionBackendKind Backend;
  ExecutionContract Contract;
  char Mode;
  std::string Name;
};

void PrintTo(const MutexParameter &Parameter, std::ostream *Stream) {
  *Stream << Parameter.Name;
}

std::vector<MutexParameter> parameters() {
  std::vector<MutexParameter> Result;
  for (auto Backend : {ExecutionBackendKind::Unicorn, ExecutionBackendKind::KVM,
                       ExecutionBackendKind::WHP})
    for (auto Contract :
         {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
      const std::string Prefix =
          std::string(executionBackendName(Backend)) +
          (Contract == ExecutionContract::Legacy ? SehMutexDriverSuffix
                                                 : SehMutexCheckedSuffix);
#define NEVERD_SEH_MUTEX_CASE(Name, Value)                                     \
  Result.push_back({Backend, Contract, Value, Prefix + #Name});
#include "fixtures/driver_seh_mutex.def"
#undef NEVERD_SEH_MUTEX_CASE
    }
  return Result;
}

class DriverMutexThread : public testing::TestWithParam<MutexParameter> {};

TEST_P(DriverMutexThread, NestedAndBlockedCallsRetainThreadOwnership) {
#ifdef NEVERD_WDM_SEH_FIXTURE
  const auto &Parameter = GetParam();
  auto Probe = createExecutionBackend(Parameter.Backend, Parameter.Contract,
                                      profile::DefaultMemoryLimit);
  if (!Probe) {
    auto Error = Probe.takeError();
    const bool Unavailable = Error.isA<BackendUnavailableError>();
    const auto Text = llvm::toString(std::move(Error));
    if (Unavailable)
      GTEST_SKIP() << Text;
    FAIL() << Text;
  }
  Probe->CPU.reset();
  std::vector<const char *> Images{NEVERD_WDM_SEH_FIXTURE};
#ifdef NEVERD_WDM_SEH_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_SEH_CFG_FIXTURE);
#endif
  for (const auto *Image : Images)
    for (uint64_t Base : {SehMutexBase, SehMutexRebased})
      for (uint64_t Quantum : {uint64_t(0), uint64_t(SehMutexTinyQuantum),
                               uint64_t(SehMutexShortQuantum)}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Base);
        SCOPED_TRACE(Quantum);
        DriverOptions Options;
        Options.Backend = Parameter.Backend;
        Options.Contract = Parameter.Contract;
        Options.ReportBackendSelection = true;
        Options.InstructionLimit = SehMutexInstructionLimit;
        Options.LoadAddress = Base;
        Options.Unload = true;
        Options.ServiceName = std::string(SehMutexService) +
                              char(SehMutexMarker) + Parameter.Mode;
        if (Quantum)
          Options.Scheduling = DriverScheduling{Quantum};
        auto Result = emulateDriver(Image, Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        EXPECT_EQ(Result->SelectedBackend, Parameter.Backend);
        EXPECT_EQ(Result->NTStatus, 0u);
        EXPECT_EQ(Result->ImageBase, Base);
        EXPECT_FALSE(Result->Fault);
        EXPECT_TRUE(Result->UnloadCompleted);
        EXPECT_TRUE(Result->Requests.empty());
        EXPECT_TRUE(Result->Devices.empty());
        EXPECT_EQ(std::count(Result->Messages.begin(), Result->Messages.end(),
                             SehMutexCompleteMessage),
                  1);
        for (const auto &Message : Result->Messages)
          EXPECT_EQ(Message.find(SehMutexFailure), std::string::npos)
              << Message;
      }
#else
  GTEST_SKIP() << SehMutexMissing;
#endif
}

INSTANTIATE_TEST_SUITE_P(Native, DriverMutexThread,
                         testing::ValuesIn(parameters()),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation
