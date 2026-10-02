//===- DriverBackendParityTests.cpp - Original workload backend parity ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <filesystem>
#include <tuple>

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

llvm::json::Value observableResult(const DriverResult &Result) {
  auto JSON = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
  auto &Object = *JSON.getAsObject();
  // Backend identity and human diagnostic wording may differ. Unicorn counts
  // an extra no-access REP termination observation; checked MOVS counts actual
  // elements. All remaining report fields, including request bytes, calls,
  // write events, CPU fault facts and final object/device state, must agree.
#define NEVERD_PARITY_REPORT_METADATA(Name) Object.erase(Name);
#include "DriverBackendParityCases.def"
#undef NEVERD_PARITY_REPORT_METADATA
  for (auto &Call : *Object.getArray(Calls)) {
    auto &Fields = *Call.getAsObject();
    if (Fields.get(ResultKey) &&
        Fields.get(ResultKey)->kind() == llvm::json::Value::Null)
      Fields.erase(Detail);
  }
  return JSON;
}

using Parameter =
    std::tuple<Workload, ExecutionBackendKind, bool, ExecutionContract>;
std::string parameterName(const testing::TestParamInfo<Parameter> &P) {
  const auto &[Input, Backend, Rebase, Contract] = P.param;
  return std::string(executionBackendName(Backend)) + Separator + Input.Name +
         (Rebase ? RebaseSuffix : OriginalSuffix) +
         (Contract == ExecutionContract::Legacy ? LegacySuffix : CheckedSuffix);
}

class DriverBackendParity : public testing::TestWithParam<Parameter> {};
TEST_P(DriverBackendParity, OriginalImageAndScenarioPreserveObservableResults) {
  const auto &[Input, Backend, Rebase, Contract] = GetParam();
  SCOPED_TRACE(Input.Name);
  if (!std::filesystem::is_regular_file(Input.Image))
    GTEST_SKIP() << UnavailableFixture;
  auto Probe = createExecutionBackend(Backend, Contract, MemoryLimit);
  if (!Probe) {
    auto E = Probe.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable && !requireHvf(Backend, GuestArchitecture::X64))
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
  const auto Software = llvm::cantFail(queryExecutionBackendBuild(
      ExecutionBackendKind::Unicorn, GuestArchitecture::X64));
  if (Software.Availability != BackendAvailability::Available)
    GTEST_SKIP() << Software.Reason;
  Options.Backend = ExecutionBackendKind::Unicorn;
  Options.Contract = ExecutionContract::Legacy;
  auto Expected = emulateDriver(Input.Image, Options);
  Options.Backend = Backend;
  Options.Contract = Contract;
  auto Actual = emulateDriver(Input.Image, Options);
  if (!Expected) {
    // Minimal fixed-address fixtures deliberately omit relocation records.
    // A rebase must be rejected by the common loader before CPU execution.
    const auto Reason = llvm::toString(Expected.takeError());
    EXPECT_TRUE(Rebase);
    EXPECT_EQ(Reason, MissingRelocations);
    ASSERT_FALSE(bool(Actual));
    EXPECT_EQ(llvm::toString(Actual.takeError()), Reason);
    return;
  }
  ASSERT_TRUE(bool(Actual)) << llvm::toString(Actual.takeError());
  EXPECT_EQ(Actual->SelectedBackend, Backend);
  EXPECT_EQ(Expected->ImageBase,
            Rebase ? RebasedAddress : Expected->PreferredImageBase);
  EXPECT_EQ(Actual->ImageBase,
            Rebase ? RebasedAddress : Actual->PreferredImageBase);
  EXPECT_EQ(observableResult(*Actual), observableResult(*Expected))
      << driverResultJSON(*Actual) << driverResultJSON(*Expected);
}
INSTANTIATE_TEST_SUITE_P(
    Corpus, DriverBackendParity,
    testing::Combine(testing::ValuesIn(Workloads),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Bool(),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64)),
    parameterName);
} // namespace
} // namespace neverd::emulation
