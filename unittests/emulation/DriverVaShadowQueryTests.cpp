//===- DriverVaShadowQueryTests.cpp - Mitigation query execution ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declaration and compiled driver lifecycle mitigation query checks.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
TEST(DriverVaShadowQueryDeclaration, RoundTripsAndPreservesExplicitZero) {
  DriverResult Result;
  auto JSON = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
  EXPECT_FALSE(JSON.getAsObject()
                   ->getObject("configuration")
                   ->get("kernel_va_shadow_query"));
  for (uint32_t Flags : {0u, 0x1234u, 0x3fffu}) {
    auto Options = driverOptionsFromScenarioJSON(
        "{\"kernel_va_shadow_query\":{\"version\":1,\"flags\":" +
        std::to_string(Flags) + ",\"evidence\":\"declared\"}}");
    ASSERT_TRUE(bool(Options)) << llvm::toString(Options.takeError());
    ASSERT_TRUE(Options->KernelVaShadowQuery);
    EXPECT_EQ(Options->KernelVaShadowQuery->Flags, Flags);
    auto Inherited = driverOptionsFromScenarioJSON("{}", *Options);
    ASSERT_TRUE(bool(Inherited));
    Result.Configuration = *Inherited;
    JSON = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
    const auto *Value = JSON.getAsObject()
                            ->getObject("configuration")
                            ->getObject("kernel_va_shadow_query");
    ASSERT_NE(Value, nullptr);
    EXPECT_EQ(Value->getInteger("version"), 1);
    EXPECT_EQ(Value->getInteger("flags"), Flags);
    EXPECT_EQ(Value->getString("evidence"), "declared");
    const auto *Declaration = JSON.getAsObject()
                                  ->getObject("configuration")
                                  ->get("kernel_va_shadow_query");
    auto RoundTrip = driverOptionsFromScenarioJSON(
        "{\"kernel_va_shadow_query\":" +
        llvm::formatv("{0}", *Declaration).str() + "}");
    ASSERT_TRUE(bool(RoundTrip));
    EXPECT_EQ(RoundTrip->KernelVaShadowQuery->Flags, Flags);
  }
}

TEST(DriverVaShadowQueryDeclaration, RejectsMalformedAndLegacyBeforeLoading) {
  const std::vector<std::string> Invalid{
      "null",
      "[]",
      "true",
      "{}",
      R"({"version":1,"evidence":"missing"})",
      R"({"flags":0,"evidence":"missing"})",
      R"({"version":1,"flags":0})",
      R"({"version":0,"flags":0,"evidence":"bad"})",
      R"({"version":2,"flags":0,"evidence":"bad"})",
      R"({"version":1.0,"flags":0,"evidence":"bad"})",
      R"({"version":1e0,"flags":0,"evidence":"bad"})",
      R"({"version":1,"flags":1.0,"evidence":"bad"})",
      R"({"version":1,"flags":1e0,"evidence":"bad"})",
      R"({"version":1,"flags":-1,"evidence":"bad"})",
      R"({"version":1,"flags":4294967296,"evidence":"bad"})",
      R"({"version":1,"flags":16384,"evidence":"bad"})",
      R"({"version":1,"flags":true,"evidence":"bad"})",
      R"({"version":1,"flags":null,"evidence":"bad"})",
      R"({"version":1,"flags":"0","evidence":"bad"})",
      R"({"version":1,"flags":0,"evidence":""})",
      R"({"version":1,"flags":0,"evidence":"bad\nlabel"})",
      R"({"version":1,"flags":0,"evidence":"\u0080"})",
      R"({"version":1,"flags":0,"evidence":7})",
      R"({"version":1,"flags":0,"evidence":"bad","extra":1})",
      R"({"version":1,"flags":0,"flags":0,"evidence":"duplicate"})",
      R"({"version":1,"flags":0,"evidence":")" + std::string(257, 'x') + "\"}"};
  for (const auto &Value : Invalid) {
    auto Parsed = driverOptionsFromScenarioJSON(
        "{\"kernel_va_shadow_query\":" + Value + "}");
    ASSERT_FALSE(bool(Parsed)) << Value;
    llvm::consumeError(Parsed.takeError());
  }
  for (auto Backend :
       {ExecutionBackendKind::Auto, ExecutionBackendKind::Unicorn,
        ExecutionBackendKind::KVM, ExecutionBackendKind::WHP}) {
    DriverOptions Options;
    Options.Backend = Backend;
    Options.KernelVaShadowQuery = DriverKernelVaShadowQuery{1, 0, "legacy"};
    auto Result = emulateDriver("absent.sys", Options);
    ASSERT_FALSE(bool(Result));
    EXPECT_EQ(llvm::toString(Result.takeError()),
              "driver kernel_va_shadow_query requires checked-x64 execution");
    Options.Contract = ExecutionContract::CheckedX64;
    Options.KernelVaShadowQuery->Flags.reset();
    Result = emulateDriver("absent.sys", Options);
    ASSERT_FALSE(bool(Result));
    EXPECT_NE(llvm::toString(Result.takeError()).find("flags must be supplied"),
              std::string::npos);
  }
}

class DriverVaShadowQueryExecution
    : public testing::TestWithParam<ExecutionBackendKind> {
protected:
  DriverOptions Options;
  std::filesystem::path Path = std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                               "driver_va_shadow_query.sys";
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

TEST_P(DriverVaShadowQueryExecution,
       ImportsAndDynamicQueriesSurviveRebasedLifecycle) {
  for (uint64_t Base : {uint64_t(0), uint64_t(0x1a0000000)})
    for (uint32_t Flags : {0u, 0x1234u, 0x3fffu}) {
      Options.LoadAddress = Base;
      Options.KernelVaShadowQuery =
          DriverKernelVaShadowQuery{1, Flags, "independent lifecycle"};
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
      EXPECT_EQ(llvm::support::endian::read32le(Run->Requests[1].Output.data()),
                Flags);
      EXPECT_NE(std::find(Run->Messages.begin(), Run->Messages.end(),
                          "mitigation query unload complete"),
                Run->Messages.end());
    }
}

TEST_P(DriverVaShadowQueryExecution, MissingDeclarationAndInvalidOutputsStop) {
  auto Run = emulateDriver(Path, Options);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  EXPECT_EQ(Run->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Run->Diagnostic.find("not declared"), std::string::npos);
  EXPECT_TRUE(Run->Requests.empty());
  Options.KernelVaShadowQuery =
      DriverKernelVaShadowQuery{1, 42, "strict query"};
  for (uint32_t Code : {0x222004u, 0x222008u}) {
    Options.Requests[1].ControlCode = Code;
    Run = emulateDriver(Path, Options);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Run->Diagnostic.find(Code == 0x222004 ? "overlap"
                                                    : "invalid buffer extent"),
              std::string::npos);
    EXPECT_FALSE(Run->UnloadCompleted);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Native, DriverVaShadowQueryExecution,
    testing::Values(ExecutionBackendKind::Unicorn, ExecutionBackendKind::KVM,
                    ExecutionBackendKind::WHP),
    [](const testing::TestParamInfo<ExecutionBackendKind> &P) {
      return std::string(executionBackendName(P.param));
    });
} // namespace
} // namespace neverd::emulation
