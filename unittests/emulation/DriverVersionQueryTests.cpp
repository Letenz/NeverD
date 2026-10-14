//===- DriverVersionQueryTests.cpp - Declared query execution and transport =//
//===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declaration and compiled driver lifecycle version query checks.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/JSON.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
TEST(DriverVersionQueryDeclaration, RoundTripsAndPreservesExplicitZero) {
  DriverResult Result;
  auto JSON = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
  EXPECT_FALSE(
      JSON.getAsObject()->getObject("configuration")->get("version_query"));
  auto Options = driverOptionsFromScenarioJSON(
      R"({"version_query":{"version":1,"build_number":0,"evidence":"declared"}})");
  ASSERT_TRUE(bool(Options)) << llvm::toString(Options.takeError());
  ASSERT_TRUE(Options->VersionQuery);
  EXPECT_EQ(Options->VersionQuery->BuildNumber, 0u);
  EXPECT_FALSE(Options->VersionQuery->AllowZeroSize);
  auto Inherited = driverOptionsFromScenarioJSON("{}", *Options);
  ASSERT_TRUE(bool(Inherited));
  Result.Configuration = *Inherited;
  JSON = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
  const auto *Query = JSON.getAsObject()
                          ->getObject("configuration")
                          ->getObject("version_query");
  ASSERT_NE(Query, nullptr);
  EXPECT_EQ(Query->getInteger("version"), 1);
  EXPECT_EQ(Query->getInteger("build_number"), 0);
  EXPECT_EQ(Query->getBoolean("allow_zero_size"), false);
  EXPECT_EQ(Query->getString("evidence"), "declared");
  auto Enabled = driverOptionsFromScenarioJSON(
      R"({"version_query":{"version":1,"build_number":65535,"evidence":"compatibility","allow_zero_size":true}})",
      *Options);
  ASSERT_TRUE(bool(Enabled));
  EXPECT_EQ(Enabled->VersionQuery->BuildNumber, 65535u);
  EXPECT_TRUE(Enabled->VersionQuery->AllowZeroSize);
}

TEST(DriverVersionQueryDeclaration, RejectsMalformedAndLegacyBeforeLoading) {
  for (
      const char *Value :
      {"null",
       "[]",
       "true",
       "{}",
       R"({"version":1,"evidence":"missing"})",
       R"({"build_number":0,"evidence":"missing"})",
       R"({"version":1,"build_number":0})",
       R"({"version":2,"build_number":0,"evidence":"bad"})",
       R"({"version":1.0,"build_number":0,"evidence":"bad"})",
       R"({"version":1,"build_number":0.0,"evidence":"bad"})",
       R"({"version":1,"build_number":1e0,"evidence":"bad"})",
       R"({"version":1,"build_number":-1,"evidence":"bad"})",
       R"({"version":1,"build_number":65536,"evidence":"bad"})",
       R"({"version":1,"build_number":true,"evidence":"bad"})",
       R"({"version":1,"build_number":null,"evidence":"bad"})",
       R"({"version":1,"build_number":0,"evidence":""})",
       R"({"version":1,"build_number":0,"evidence":"bad\nlabel"})",
       R"({"version":1,"build_number":0,"evidence":7})",
       R"({"version":1,"build_number":0,"evidence":"bad","allow_zero_size":1})",
       R"({"version":1,"build_number":0,"evidence":"bad","allow_zero_size":null})",
       R"({"version":1,"build_number":0,"evidence":"bad","extra":1})",
       R"({"version":1,"build_number":0,"build_number":1,"evidence":"bad"})"}) {
    auto Parsed = driverOptionsFromScenarioJSON(
        std::string("{\"version_query\":") + Value + "}");
    ASSERT_FALSE(bool(Parsed)) << Value;
    llvm::consumeError(Parsed.takeError());
  }
  for (auto Backend :
       {ExecutionBackendKind::Auto, ExecutionBackendKind::Unicorn,
        ExecutionBackendKind::KVM, ExecutionBackendKind::WHP}) {
    DriverOptions Options;
    Options.Backend = Backend;
    Options.VersionQuery = DriverVersionQuery{1, 0, "legacy", true};
    auto Result = emulateDriver("absent.sys", Options);
    ASSERT_FALSE(bool(Result));
    EXPECT_EQ(llvm::toString(Result.takeError()),
              "driver version_query requires checked-x64 execution");
  }
}

class DriverVersionQueryExecution
    : public testing::TestWithParam<ExecutionBackendKind> {
protected:
  DriverOptions Options;
  std::filesystem::path Path = std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                               "driver_version_query.sys";
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
    ASSERT_TRUE(std::filesystem::is_regular_file(Path));
    Options.Unload = true;
    for (auto Kind :
         {DriverRequestKind::Create, DriverRequestKind::DeviceControl,
          DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
      DriverRequest Request;
      Request.Kind = Kind;
      if (Kind == DriverRequestKind::DeviceControl) {
        Request.ControlCode = 0x222000;
        Request.OutputSize = 4;
      }
      Options.Requests.push_back(Request);
    }
  }
};

TEST_P(DriverVersionQueryExecution,
       ImportsAndDynamicQueriesSurviveRebasedLifecycle) {
  for (uint64_t Base : {uint64_t(0), uint64_t(0x1a0000000)})
    for (uint32_t Build : {0u, 65535u})
      for (bool Zero : {false, true}) {
        Options.LoadAddress = Base;
        Options.VersionQuery =
            DriverVersionQuery{1, Build, "independent lifecycle", Zero};
        Options.Requests[1].ControlCode = Zero ? 0x222004 : 0x222000;
        auto Run = emulateDriver(Path, Options);
        ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
        ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
        EXPECT_EQ(Run->NTStatus, 0u);
        EXPECT_TRUE(Run->UnloadCompleted);
        EXPECT_TRUE(Run->Devices.empty());
        ASSERT_EQ(Run->Requests.size(), 4u);
        for (const auto &Request : Run->Requests) {
          EXPECT_TRUE(Request.Completed);
          EXPECT_EQ(Request.IOStatus, 0u);
        }
        ASSERT_EQ(Run->Requests[1].Output.size(), 4u);
        EXPECT_EQ(
            llvm::support::endian::read32le(Run->Requests[1].Output.data()),
            Build);
        EXPECT_NE(std::find(Run->Messages.begin(), Run->Messages.end(),
                            "version query unload complete"),
                  Run->Messages.end());
      }
}

TEST_P(DriverVersionQueryExecution,
       MissingDeclarationAndUnsupportedShapesStop) {
  auto Run = emulateDriver(Path, Options);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  EXPECT_EQ(Run->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Run->Diagnostic.find("not declared"), std::string::npos);
  EXPECT_TRUE(Run->Requests.empty());
  Options.VersionQuery = DriverVersionQuery{1, 42, "strict query", false};
  for (uint32_t Code : {0x222004u, 0x222008u}) {
    Options.Requests[1].ControlCode = Code;
    Run = emulateDriver(Path, Options);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Run->Diagnostic.find("size is unsupported"), std::string::npos);
    EXPECT_FALSE(Run->UnloadCompleted);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Native, DriverVersionQueryExecution,
    testing::Values(ExecutionBackendKind::Unicorn, ExecutionBackendKind::KVM,
                    ExecutionBackendKind::WHP),
    [](const testing::TestParamInfo<ExecutionBackendKind> &P) {
      return std::string(executionBackendName(P.param));
    });
} // namespace
} // namespace neverd::emulation
