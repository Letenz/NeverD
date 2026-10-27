//===- DriverProcessPebTests.cpp - Declared process query execution
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <filesystem>

namespace neverd::emulation {
namespace {
TEST(DriverProcessPebDeclaration, RoundTripsExactValuesAndExplicitAbsence) {
  for (uint64_t Value : {0ULL, 0xffffffffffffffffULL, 0x123456789abcdef0ULL}) {
    const auto Text =
        std::string(R"({"process_pebs":[{"process_id":4,"value":")") +
        llvm::formatv("{0:x}", Value).str() + "\"}]}";
    auto Parsed = driverOptionsFromScenarioJSON(Text);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    ASSERT_EQ(Parsed->ProcessPebs.size(), 1u);
    EXPECT_EQ(Parsed->ProcessPebs[0].ProcessID, 4u);
    EXPECT_EQ(Parsed->ProcessPebs[0].Value, Value);
    auto Inherited = driverOptionsFromScenarioJSON("{}", *Parsed);
    ASSERT_TRUE(bool(Inherited));
    DriverResult Result;
    Result.Configuration = *Inherited;
    auto Report = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
    const auto *Entries =
        Report.getAsObject()->getObject("configuration")->get("process_pebs");
    ASSERT_NE(Entries, nullptr);
    auto RoundTrip = driverOptionsFromScenarioJSON(
        "{\"process_pebs\":" + llvm::formatv("{0}", *Entries).str() + "}");
    ASSERT_TRUE(bool(RoundTrip)) << llvm::toString(RoundTrip.takeError());
    ASSERT_EQ(RoundTrip->ProcessPebs.size(), 1u);
    EXPECT_EQ(RoundTrip->ProcessPebs[0].Value, Value);
    auto Cleared =
        driverOptionsFromScenarioJSON(R"({"process_pebs":[]})", *Parsed);
    ASSERT_TRUE(bool(Cleared));
    EXPECT_TRUE(Cleared->ProcessPebs.empty());
  }
  for (const char *ID : {"4.0", "4e0", "4294967295"}) {
    auto Parsed = driverOptionsFromScenarioJSON(
        std::string(R"({"process_pebs":[{"process_id":)") + ID +
        R"(,"value":"0x0"}]})");
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  }
}

TEST(DriverProcessPebDeclaration, RejectsMalformedAndLegacyInputs) {
  for (const char *Value :
       {"null",
        "{}",
        "true",
        "[0]",
        "[{}]",
        R"([{"process_id":4}])",
        R"([{"value":"0x0"}])",
        R"([{"process_id":0,"value":"0x0"}])",
        R"([{"process_id":-1,"value":"0x0"}])",
        R"([{"process_id":4294967296,"value":"0x0"}])",
        R"([{"process_id":4.5,"value":"0x0"}])",
        R"([{"process_id":"4","value":"0x0"}])",
        R"([{"process_id":true,"value":"0x0"}])",
        R"([{"process_id":4,"value":0}])",
        R"([{"process_id":4,"value":null}])",
        R"([{"process_id":4,"value":"0"}])",
        R"([{"process_id":4,"value":"0x"}])",
        R"([{"process_id":4,"value":"0x10000000000000000"}])",
        R"([{"process_id":4,"value":"0xg"}])",
        R"([{"process_id":4,"value":"0x1 ","extra":1}])",
        R"([{"process_id":4,"value":"0x0"},{"process_id":4,"value":"0x1"}])",
        R"([{"process_id":4,"process_id":8,"value":"0x0"}])"}) {
    auto Parsed = driverOptionsFromScenarioJSON(
        std::string("{\"process_pebs\":") + Value + "}");
    ASSERT_FALSE(bool(Parsed)) << Value;
    llvm::consumeError(Parsed.takeError());
  }
  DriverOptions Options;
  Options.ProcessPebs.resize(profile::MaxProcessPebEntries + 1, {4, 0});
  auto Excess = driverOptionsFromScenarioJSON("{}", Options);
  ASSERT_FALSE(bool(Excess));
  llvm::consumeError(Excess.takeError());
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::DirectDriverX64}) {
    Options = {};
    Options.Contract = Contract;
    Options.TraceMemoryWrites = false;
    Options.ProcessPebs = {{4, 0}};
    auto Run = emulateDriver("absent.sys", Options);
    ASSERT_FALSE(bool(Run));
    EXPECT_NE(llvm::toString(Run.takeError())
                  .find("process_pebs requires checked-x64"),
              std::string::npos);
  }
}

class DriverProcessPebExecution
    : public testing::TestWithParam<ExecutionBackendKind> {
protected:
  DriverOptions Options;
  void SetUp() override {
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
  }
  llvm::Expected<DriverResult> run() {
    return emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                             "driver_process_peb.sys",
                         Options);
  }
};

TEST_P(DriverProcessPebExecution, NullDeclarationAndRepeatedQueriesAreExact) {
  for (uint64_t Base : {0ULL, 0x1c0000000ULL}) {
    Options.LoadAddress = Base;
    Options.ProcessPebs = {{4, 0}};
    auto Run = run();
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
    EXPECT_EQ(Run->NTStatus, 0u);
    size_t Queries = 0;
    for (const auto &Call : Run->Calls)
      if (Call.Name == "PsGetProcessPeb") {
        ++Queries;
        EXPECT_EQ(Call.Result, 0u);
      }
    EXPECT_EQ(Queries, 2u);
  }
}

TEST_P(DriverProcessPebExecution, MissingAndForeignDeclarationsStop) {
  for (bool Foreign : {false, true}) {
    Options.ProcessPebs = Foreign ? std::vector<DriverProcessPeb>{{8, 0}}
                                  : std::vector<DriverProcessPeb>{};
    auto Run = run();
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, DriverStopReason::ModelError) << Run->Diagnostic;
    EXPECT_NE(Run->Diagnostic.find("undeclared process identity"),
              std::string::npos);
    EXPECT_FALSE(Run->NTStatus.has_value());
  }
}

TEST_P(DriverProcessPebExecution, ValuesGrantNeitherStorageNorProcessIdentity) {
  for (uint64_t Value : {1ULL, 0x50000000ULL, 0xffff800050000000ULL}) {
    Options.ProcessPebs = {{4, Value}};
    auto Run = run();
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, Value == 1 ? DriverStopReason::ModelError
                                    : DriverStopReason::MemoryFault)
        << Run->Diagnostic;
    size_t Replies = 0;
    for (const auto &Call : Run->Calls)
      if (Call.Name == "PsGetProcessPeb" && Call.Result) {
        ++Replies;
        EXPECT_EQ(Call.Result, Value);
      }
    EXPECT_EQ(Replies, 2u);
    if (Value == 1)
      EXPECT_NE(Run->Diagnostic.find("known process object"),
                std::string::npos);
    EXPECT_FALSE(Run->NTStatus.has_value());
  }
}

INSTANTIATE_TEST_SUITE_P(
    Native, DriverProcessPebExecution,
    testing::Values(ExecutionBackendKind::Unicorn, ExecutionBackendKind::KVM,
                    ExecutionBackendKind::WHP, ExecutionBackendKind::HVF),
    [](const testing::TestParamInfo<ExecutionBackendKind> &Info) {
      return std::string(executionBackendName(Info.param));
    });
} // namespace
} // namespace neverd::emulation
