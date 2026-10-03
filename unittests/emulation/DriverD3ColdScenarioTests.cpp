//===- DriverD3ColdScenarioTests.cpp - Explicit cold supply facts -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validate explicit cold-power facts and native/JSON configuration parity.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "os/windows/driver/DriverScenario.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {
constexpr llvm::StringLiteral ColdFacts =
    R"({"supported":true,"enabled_by_default":false,"wake_s0":true,"wake_sx":false})";
std::string scenario(llvm::StringRef Cold = ColdFacts) {
  return R"({"pnp_devices":[{"id":"cold-pdo","bus":"resource_free",
    "initial_device_power":"D0","initial_system_power":"working",
    "wake_capabilities":{"s0":true,"sx":true},"d3cold":)" +
         Cold.str() + "}]}";
}

TEST(DriverD3ColdScenario, ParsesDedicatedSupplyWithoutInventingDefaultPolicy) {
  auto Options = driverOptionsFromScenarioJSON(scenario());
  ASSERT_TRUE(bool(Options)) << llvm::toString(Options.takeError());
  ASSERT_EQ(Options->PnpDevices.size(), 1u);
  ASSERT_TRUE(Options->PnpDevices[0].D3Cold);
  const auto &Cold = *Options->PnpDevices[0].D3Cold;
  EXPECT_TRUE(Cold.Supported);
  EXPECT_FALSE(Cold.EnabledByDefault);
  EXPECT_TRUE(Cold.WakeS0);
  EXPECT_FALSE(Cold.WakeSx);
  DriverResult Result;
  Result.Configuration = *Options;
  auto JSON = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
  const auto *Configuration = JSON->getAsObject()->getObject("configuration");
  ASSERT_NE(Configuration, nullptr);
  const auto *Devices = Configuration->getArray("pnp_devices");
  ASSERT_NE(Devices, nullptr);
  const auto *Reported = Devices->front().getAsObject()->getObject("d3cold");
  ASSERT_NE(Reported, nullptr);
  EXPECT_EQ(Reported->getBoolean("supported"), true);
  EXPECT_EQ(Reported->getBoolean("enabled_by_default"), false);
  EXPECT_EQ(Reported->getBoolean("wake_s0"), true);
  EXPECT_EQ(Reported->getBoolean("wake_sx"), false);
}

TEST(DriverD3ColdScenario, RequiresExplicitBooleansAndRejectsUnknownFacts) {
  for (
      const auto Cold :
      {"false", "null", "{}",
       R"({"supported":true,"enabled_by_default":false,"wake_s0":true})",
       R"({"supported":1,"enabled_by_default":false,"wake_s0":true,"wake_sx":false})",
       R"({"supported":true,"enabled_by_default":false,"wake_s0":true,"wake_sx":false,"rail":"shared"})"}) {
    SCOPED_TRACE(Cold);
    auto Options = driverOptionsFromScenarioJSON(scenario(Cold));
    ASSERT_FALSE(bool(Options));
    llvm::consumeError(Options.takeError());
  }
}

TEST(DriverD3ColdScenario, NativeAndJSONRejectContradictoryColdCapabilities) {
  for (
      const auto Cold :
      {R"({"supported":false,"enabled_by_default":true,"wake_s0":false,"wake_sx":false})",
       R"({"supported":false,"enabled_by_default":false,"wake_s0":true,"wake_sx":false})"}) {
    auto Options = driverOptionsFromScenarioJSON(scenario(Cold));
    ASSERT_FALSE(bool(Options));
    llvm::consumeError(Options.takeError());
  }
  auto Options = driverOptionsFromScenarioJSON(scenario());
  ASSERT_TRUE(bool(Options)) << llvm::toString(Options.takeError());
  Options->PnpDevices[0].WakeCapabilities.reset();
  auto Error = validateDriverScenario(*Options);
  ASSERT_TRUE(bool(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("wake_capabilities"),
            std::string::npos);
  Options->PnpDevices[0].D3Cold->WakeS0 = false;
  Error = validateDriverScenario(*Options);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
}

TEST(DriverD3ColdScenario, AbsentColdFactsKeepLegacyScenarioSemantics) {
  auto Options = driverOptionsFromScenarioJSON(
      R"({"pnp_devices":[{"id":"hot-pdo","bus":"resource_free",
        "initial_device_power":"D0","initial_system_power":"working"}]})");
  ASSERT_TRUE(bool(Options)) << llvm::toString(Options.takeError());
  EXPECT_FALSE(Options->PnpDevices[0].D3Cold);
}

} // namespace
} // namespace neverd::emulation
