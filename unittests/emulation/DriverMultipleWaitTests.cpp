//===- DriverMultipleWaitTests.cpp - Original multi-object wait execution -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include <algorithm>
#include <ostream>

namespace neverd::emulation {
namespace {
#define NEVERD_MULTI_WAIT_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_MULTI_WAIT_TEXT(Name, Value) constexpr char Name[] = Value;
#include "fixtures/DriverMultipleWaitCases.def"
#undef NEVERD_MULTI_WAIT_TEXT
#undef NEVERD_MULTI_WAIT_VALUE

struct WaitParameter {
  ExecutionBackendKind Backend;
  ExecutionContract Contract;
  char Mode;
  std::string Name;
};
void PrintTo(const WaitParameter &Parameter, std::ostream *Stream) {
  *Stream << Parameter.Name;
}
std::vector<WaitParameter> parameters() {
  std::vector<WaitParameter> Result;
  for (auto Backend : {ExecutionBackendKind::Unicorn, ExecutionBackendKind::KVM,
                       ExecutionBackendKind::WHP})
    for (auto Contract :
         {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
      const std::string Prefix =
          std::string(executionBackendName(Backend)) +
          (Contract == ExecutionContract::Legacy ? LegacySuffix
                                                 : CheckedSuffix);
#define NEVERD_MULTI_WAIT_CASE(Name, Value)                                    \
  Result.push_back({Backend, Contract, Value, Prefix + #Name});
#include "fixtures/DriverMultipleWaitCases.def"
#undef NEVERD_MULTI_WAIT_CASE
    }
  return Result;
}

class DriverMultipleWait : public testing::TestWithParam<WaitParameter> {};

TEST_P(DriverMultipleWait, OriginalWaitSetsPreserveSignalsAndThreadLifetimes) {
#ifdef NEVERD_WDM_MULTIPLE_WAIT_FIXTURE
  const auto &Parameter = GetParam();
  auto Probe = createExecutionBackend(Parameter.Backend, Parameter.Contract,
                                      MemoryLimit);
  if (!Probe) {
    auto Error = Probe.takeError();
    const bool Unavailable = Error.isA<BackendUnavailableError>();
    const auto Message = llvm::toString(std::move(Error));
    if (Unavailable)
      GTEST_SKIP() << Message;
    FAIL() << Message;
  }
  Probe->CPU.reset();
  std::vector<const char *> Images{NEVERD_WDM_MULTIPLE_WAIT_FIXTURE};
#ifdef NEVERD_WDM_MULTIPLE_WAIT_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_MULTIPLE_WAIT_CFG_FIXTURE);
#endif
  for (const auto *Image : Images)
    for (uint64_t Base : {ImageBase, Rebased})
      for (uint64_t Quantum : {uint64_t(0), uint64_t(1), SmallQuantum}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Base);
        SCOPED_TRACE(Quantum);
        DriverOptions Options;
        Options.Backend = Parameter.Backend;
        Options.Contract = Parameter.Contract;
        Options.ReportBackendSelection = true;
        Options.InstructionLimit = InstructionLimit;
        Options.LoadAddress = Base;
        Options.Unload = true;
        Options.ServiceName =
            std::string(Service) + char(ModeMarker) + Parameter.Mode;
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
                             Complete),
                  1);
      }
#else
  GTEST_SKIP() << MissingFixture;
#endif
}

INSTANTIATE_TEST_SUITE_P(
    Native, DriverMultipleWait, testing::ValuesIn(parameters()),
    [](const testing::TestParamInfo<WaitParameter> &Parameter) {
      return Parameter.param.Name;
    });
} // namespace
} // namespace neverd::emulation
