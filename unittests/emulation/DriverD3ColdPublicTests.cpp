//===- DriverD3ColdPublicTests.cpp - Cold power through the public C API -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validate genuine KMDF cold-power recovery through the public scenario API.
///
//===----------------------------------------------------------------------===//

#include "fixtures/driver_kmdf_d3cold_scenario.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIEmulation.h"

#include "llvm/Support/JSON.h"

#include <memory>
#include <type_traits>

namespace {
TEST(DriverD3ColdPublic, CAPIExecutesColdWakeAndReportsProviderFacts) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  const auto Scenario =
      neverd::test::kmdfD3ColdScenario(KmdfPowerColdExplicit, false, true);
  auto Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  std::unique_ptr<std::remove_pointer_t<neverd_session_t>,
                  decltype(&neverd_session_destroy)>
      Owner(Session, neverd_session_destroy);
  for (const char *Image : {
           NEVERD_KMDF_PNP_FIXTURE,
#ifdef NEVERD_KMDF_PNP_CFG_FIXTURE
           NEVERD_KMDF_PNP_CFG_FIXTURE,
#endif
       }) {
    const char *Text = neverd_emulate_driver_scenario_json(
        Session, Image, Scenario.c_str(), nullptr);
    ASSERT_NE(Text, nullptr);
    const std::string Report(Text);
    neverd_free_string(Text);
    auto JSON = llvm::json::parse(Report);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
    const auto *Root = JSON->getAsObject();
    ASSERT_NE(Root, nullptr);
    EXPECT_EQ(Root->getString("stop_reason"), "returned") << Report;
    EXPECT_EQ(Root->getBoolean("scenario_success"), true) << Report;
    EXPECT_EQ(Root->getBoolean("unload_completed"), true);
    const auto *Configuration = Root->getObject("configuration");
    ASSERT_NE(Configuration, nullptr);
    const auto *Devices = Configuration->getArray("pnp_devices");
    ASSERT_NE(Devices, nullptr);
    const auto *Cold = Devices->front().getAsObject()->getObject("d3cold");
    ASSERT_NE(Cold, nullptr);
    EXPECT_EQ(Cold->getBoolean("supported"), true);
    EXPECT_EQ(Cold->getBoolean("wake_s0"), true);
    EXPECT_EQ(Cold->getBoolean("enabled_by_default"), false);
    const auto *Requests = Root->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    unsigned Index = 0;
    bool Observed = false;
    for (const auto &Request : *Requests) {
      const auto *Row = Request.getAsObject();
      ASSERT_NE(Row, nullptr);
      if (Row->getString("origin") != "scenario" || Index++ != 3)
        continue;
      EXPECT_EQ(
          Row->getString("output_hex"),
          "0200000001000000010000000100000001000000010000007856341201000000");
      Observed = true;
    }
    EXPECT_TRUE(Observed);
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}
} // namespace
