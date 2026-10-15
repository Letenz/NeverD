//===- DriverPhysicalInventoryTests.cpp - RAM query execution/transport
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// RAM query execution/transport.
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
DriverPhysicalMemoryInventory declaration() {
  return {1, DriverPhysicalMemoryInventory::SourceKind::ModelIdentityWindow,
          "independent simulated inventory"};
}
TEST(DriverPhysicalInventoryDeclaration, RoundTripsAndInheritsExplicitSource) {
  DriverResult Result;
  auto JSON = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
  EXPECT_FALSE(JSON.getAsObject()
                   ->getObject("configuration")
                   ->get("physical_memory_inventory"));
  auto Options = driverOptionsFromScenarioJSON(
      R"({"physical_memory_inventory":{"version":1,"source":"model-identity-window","evidence":"declared"}})");
  ASSERT_TRUE(bool(Options)) << llvm::toString(Options.takeError());
  ASSERT_TRUE(Options->PhysicalMemoryInventory);
  EXPECT_EQ(Options->PhysicalMemoryInventory->Source,
            DriverPhysicalMemoryInventory::SourceKind::ModelIdentityWindow);
  auto Inherited = driverOptionsFromScenarioJSON("{}", *Options);
  ASSERT_TRUE(bool(Inherited));
  Result.Configuration = *Inherited;
  JSON = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
  const auto *Value = JSON.getAsObject()
                          ->getObject("configuration")
                          ->getObject("physical_memory_inventory");
  ASSERT_NE(Value, nullptr);
  EXPECT_EQ(Value->getInteger("version"), 1);
  EXPECT_EQ(Value->getString("source"), "model-identity-window");
  EXPECT_EQ(Value->getString("evidence"), "declared");
}

TEST(DriverPhysicalInventoryDeclaration,
     RejectsMalformedAndLegacyBeforeLoading) {
  for (
      const char *Value :
      {"null",
       "[]",
       "true",
       "{}",
       R"({"version":1,"evidence":"missing"})",
       R"({"source":"model-identity-window","evidence":"missing"})",
       R"({"version":1,"source":"model-identity-window"})",
       R"({"version":2,"source":"model-identity-window","evidence":"bad"})",
       R"({"version":1.0,"source":"model-identity-window","evidence":"bad"})",
       R"({"version":1e0,"source":"model-identity-window","evidence":"bad"})",
       R"({"version":-1,"source":"model-identity-window","evidence":"bad"})",
       R"({"version":4294967296,"source":"model-identity-window","evidence":"bad"})",
       R"({"version":true,"source":"model-identity-window","evidence":"bad"})",
       R"({"version":1,"source":null,"evidence":"bad"})",
       R"({"version":1,"source":"host","evidence":"bad"})",
       R"({"version":1,"source":"model-identity-window","evidence":""})",
       R"({"version":1,"source":"model-identity-window","evidence":"bad\nlabel"})",
       R"({"version":1,"source":"model-identity-window","evidence":"\u0080"})",
       R"({"version":1,"source":"model-identity-window","evidence":7})",
       R"({"version":1,"source":"model-identity-window","evidence":"bad","extra":1})",
       R"({"version":1,"source":"model-identity-window","source":"model-identity-window","evidence":"duplicate"})"}) {
    auto Parsed = driverOptionsFromScenarioJSON(
        std::string("{\"physical_memory_inventory\":") + Value + "}");
    ASSERT_FALSE(bool(Parsed)) << Value;
    llvm::consumeError(Parsed.takeError());
  }
  for (auto Backend :
       {ExecutionBackendKind::Auto, ExecutionBackendKind::Unicorn,
        ExecutionBackendKind::KVM, ExecutionBackendKind::WHP}) {
    DriverOptions Options;
    Options.Backend = Backend;
    Options.PhysicalMemoryInventory = declaration();
    auto Result = emulateDriver("absent.sys", Options);
    ASSERT_FALSE(bool(Result));
    EXPECT_EQ(
        llvm::toString(Result.takeError()),
        "driver physical_memory_inventory requires checked-x64 execution");
  }
}

class DriverPhysicalInventoryExecution
    : public testing::TestWithParam<ExecutionBackendKind> {
protected:
  DriverOptions Options;
  std::filesystem::path Path = std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                               "driver_physical_ranges.sys";
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
        Request.OutputSize = 32;
      }
      Options.Requests.push_back(Request);
    }
  }
};

TEST_P(DriverPhysicalInventoryExecution,
       IndependentResultsSurviveRebasedLifecycle) {
  for (uint64_t Base : {uint64_t(0), uint64_t(0x1a0000000)}) {
    Options.LoadAddress = Base;
    Options.PhysicalMemoryInventory = declaration();
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
    const auto &Output = Run->Requests[1].Output;
    ASSERT_EQ(Output.size(), 32u);
    EXPECT_EQ(llvm::support::endian::read64le(Output.data()), 0x1000000000u);
    EXPECT_EQ(llvm::support::endian::read64le(Output.data() + 8), 0x4000000u);
    EXPECT_TRUE(std::all_of(Output.begin() + 16, Output.end(),
                            [](uint8_t B) { return B == 0; }));
    EXPECT_NE(std::find(Run->Messages.begin(), Run->Messages.end(),
                        "physical ranges unload complete"),
              Run->Messages.end());
  }
}

TEST_P(DriverPhysicalInventoryExecution, MissingDeclarationAndFreedReadsStop) {
  auto Run = emulateDriver(Path, Options);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  EXPECT_EQ(Run->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Run->Diagnostic.find("not declared"), std::string::npos);
  EXPECT_TRUE(Run->Requests.empty());
  Options.PhysicalMemoryInventory = declaration();
  Options.Requests[1].ControlCode = 0x222004;
  Run = emulateDriver(Path, Options);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  EXPECT_EQ(Run->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Run->Diagnostic.find("freed"), std::string::npos);
  EXPECT_FALSE(Run->UnloadCompleted);
}

INSTANTIATE_TEST_SUITE_P(
    Native, DriverPhysicalInventoryExecution,
    testing::Values(ExecutionBackendKind::Unicorn, ExecutionBackendKind::KVM,
                    ExecutionBackendKind::WHP),
    [](const testing::TestParamInfo<ExecutionBackendKind> &P) {
      return std::string(executionBackendName(P.param));
    });
} // namespace
} // namespace neverd::emulation
