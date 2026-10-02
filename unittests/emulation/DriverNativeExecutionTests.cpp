//===- DriverNativeExecutionTests.cpp - Native Windows corpus outcomes ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverReportFields.h"
#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <filesystem>
#include <ostream>
#include <vector>

namespace neverd::emulation {
namespace {
#define NEVERD_PARITY_IMAGE(Name, Path) constexpr char Name[] = Path;
#define NEVERD_PARITY_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_PARITY_TEXT(Name, Text) constexpr char Name[] = Text;
#include "DriverBackendParityCases.def"
#undef NEVERD_PARITY_TEXT
#undef NEVERD_PARITY_VALUE
#undef NEVERD_PARITY_IMAGE
struct Workload {
  const char *Name;
  const char *Image;
  const char *Scenario;
};
const Workload Workloads[] = {
#define NEVERD_PARITY_IMAGE(Name, Path) {#Name, Name, nullptr},
#define NEVERD_PARITY_SCENARIO(Name, Image, Scenario)                          \
  {#Name, Image, NEVERD_DRIVER_SCENARIOS "/" Scenario},
#include "DriverBackendParityCases.def"
#undef NEVERD_PARITY_SCENARIO
#undef NEVERD_PARITY_IMAGE
};

struct Parameter {
  Workload Input;
  ExecutionBackendKind Backend;
  bool Rebase;
};
void PrintTo(const Parameter &P, std::ostream *OS) {
  *OS << executionBackendName(P.Backend)
      << (P.Rebase ? RebaseSuffix : OriginalSuffix) << Separator
      << P.Input.Name;
}
std::string parameterName(const testing::TestParamInfo<Parameter> &P) {
  return testing::PrintToString(P.param);
}
std::vector<Parameter> parameters() {
  std::vector<Parameter> Result;
  for (const auto &Input : Workloads)
    for (const auto Backend :
         {ExecutionBackendKind::KVM, ExecutionBackendKind::WHP})
      for (const bool Rebase : {false, true})
        Result.push_back({Input, Backend, Rebase});
  return Result;
}

class DriverNativeExecution : public testing::TestWithParam<Parameter> {};
TEST_P(DriverNativeExecution,
       OriginalCorpusPreservesKnownOutcomesWithoutPortableTransport) {
  const auto &[Input, Backend, Rebase] = GetParam();
  SCOPED_TRACE(Input.Name);
  if (!std::filesystem::is_regular_file(Input.Image))
    GTEST_SKIP() << UnavailableFixture;
  auto Probe =
      createExecutionBackend(Backend, ExecutionContract::Legacy, MemoryLimit);
  if (!Probe) {
    auto E = Probe.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  DriverOptions Options;
  if (Input.Scenario) {
    auto Buffer = llvm::MemoryBuffer::getFile(Input.Scenario);
    ASSERT_TRUE(bool(Buffer)) << Buffer.getError().message();
    auto Parsed = driverOptionsFromScenarioJSON((*Buffer)->getBuffer());
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    Options = std::move(*Parsed);
  }
  Options.ReportBackendSelection = true;
  Options.InstructionLimit =
      std::filesystem::path(Input.Image).filename() == LoopImage
          ? LoopLimit
          : InstructionLimit;
  Options.TimeoutMilliseconds = TimeoutMilliseconds;
  // The scenario may already request a relocation. Both variants must use
  // their declared base instead of inheriting that example-specific address.
  Options.LoadAddress = Rebase ? RebasedAddress : PreferredAddress;
  Options.Backend = Backend;
  Options.Contract = ExecutionContract::Legacy;
  auto Actual = emulateDriver(Input.Image, Options);
  if (!Actual) {
    EXPECT_TRUE(Rebase);
    EXPECT_EQ(llvm::toString(Actual.takeError()), MissingRelocations);
    return;
  }
  ASSERT_EQ(Actual->SelectedBackend, Backend);
  EXPECT_EQ(Actual->ImageBase,
            Rebase ? RebasedAddress : Actual->PreferredImageBase);
  EXPECT_EQ(Actual->Configuration.Contract, ExecutionContract::Legacy);
  DriverStopReason Stop = DriverStopReason::Returned;
  std::optional<uint32_t> Status = 0;
  bool Success = true;
#define NEVERD_NATIVE_DRIVER_OUTCOME(CaseName, ExpectedStop, ExpectedStatus,   \
                                     ExpectedSuccess)                          \
  if (llvm::StringRef(Input.Name) == #CaseName) {                              \
    Stop = DriverStopReason::ExpectedStop;                                     \
    Status = ExpectedStatus;                                                   \
    Success = ExpectedSuccess;                                                 \
  }
#include "DriverNativeOutcomes.def"
#undef NEVERD_NATIVE_DRIVER_OUTCOME
  EXPECT_EQ(Actual->Stop, Stop) << driverResultJSON(*Actual);
  EXPECT_EQ(Actual->NTStatus, Status);
  const auto Report =
      llvm::cantFail(llvm::json::parse(driverResultJSON(*Actual)));
  EXPECT_EQ(Report.getAsObject()->getBoolean(field::ScenarioSuccess), Success);
}
INSTANTIATE_TEST_SUITE_P(Corpus, DriverNativeExecution,
                         testing::ValuesIn(parameters()), parameterName);
} // namespace
} // namespace neverd::emulation
