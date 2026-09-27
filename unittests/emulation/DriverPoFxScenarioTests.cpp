//===- DriverPoFxScenarioTests.cpp - Explicit component policy facts -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validate explicit component and device power decisions.
///
//===----------------------------------------------------------------------===//
#include "DriverScenario.h"
#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {
std::string scenario(llvm::StringRef Decision) {
  return R"({"pnp_devices":[{"id":"pofx","bus":"resource_free",
    "initial_device_power":"D0","initial_system_power":"working"}],
    "requests":[{"kind":"create","device_id":"pofx","file":1},
      {"kind":"ioctl","device_id":"pofx","file":1,"code":"0x222000",
       "power_policy_events":[{"after_100ns":1,"device_id":"pofx",)" +
         Decision.str() + "}]}]}";
}

TEST(DriverPoFxScenario, ComponentDecisionRoundTripsItsExactIdentityAndTarget) {
  auto Options = driverOptionsFromScenarioJSON(
      scenario(R"("action":"component_idle_state","component":0,"state":1)"));
  ASSERT_TRUE(bool(Options)) << llvm::toString(Options.takeError());
  ASSERT_EQ(Options->Requests[1].PowerPolicyEvents.size(), 1u);
  const auto &Event = Options->Requests[1].PowerPolicyEvents[0];
  EXPECT_EQ(Event.Action, DriverPowerPolicyAction::ComponentIdleState);
  EXPECT_EQ(Event.Component, 0u);
  EXPECT_EQ(Event.State, 1u);
  DriverResult Result;
  Result.Configuration = *Options;
  Result.PowerPolicyEvents.push_back(
      {1, 0, "pofx", Event.Action, 10, 10, 2, Event.Component, Event.State});
  auto JSON = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
  const auto *Root = JSON->getAsObject();
  ASSERT_NE(Root, nullptr);
  for (const auto *Events :
       {Root->getArray("power_policy_events"),
        Root->getObject("configuration")->getArray("power_policy_events")}) {
    ASSERT_NE(Events, nullptr);
    ASSERT_EQ(Events->size(), 1u);
    const auto *Item = Events->front().getAsObject();
    ASSERT_NE(Item, nullptr);
    EXPECT_EQ(Item->getString("action"), "component_idle_state");
    EXPECT_EQ(Item->getInteger("component"), 0);
    EXPECT_EQ(Item->getInteger("state"), 1);
  }
}

TEST(DriverPoFxScenario, DevicePermissionDoesNotInventAComponent) {
  auto Options = driverOptionsFromScenarioJSON(
      scenario(R"("action":"power_not_required")"));
  ASSERT_TRUE(bool(Options)) << llvm::toString(Options.takeError());
  const auto &Event = Options->Requests[1].PowerPolicyEvents[0];
  EXPECT_EQ(Event.Action, DriverPowerPolicyAction::PowerNotRequired);
  EXPECT_FALSE(Event.Component);
  EXPECT_FALSE(Event.State);
}

TEST(DriverPoFxScenario,
     ExactActionFieldsRejectMissingSurplusAndMalformedValues) {
  for (
      llvm::StringRef Decision :
      {R"("action":"component_idle_state")",
       R"("action":"component_idle_state","component":0)",
       R"("action":"component_idle_state","state":0)",
       R"("action":"component_idle_state","component":null,"state":1)",
       R"("action":"component_idle_state","component":-1,"state":1)",
       R"("action":"component_idle_state","component":0,"state":4294967296)",
       R"("action":"component_idle_state","component":false,"state":1)",
       R"("action":"component_idle_state","component":0,"state":1.5)",
       R"("action":"power_not_required","component":0,"state":0)",
       R"("action":"idle","component":0)", R"("action":"wake","state":0)",
       R"("action":"component_idle_state","component":0,"state":0,"latency":1)"}) {
    SCOPED_TRACE(Decision.str());
    auto Options = driverOptionsFromScenarioJSON(scenario(Decision));
    ASSERT_FALSE(bool(Options));
    llvm::consumeError(Options.takeError());
  }
}

TEST(DriverPoFxScenario, NativeEventsUseTheSameActionFieldContract) {
  DriverPowerPolicyEvent Event;
  Event.Action = DriverPowerPolicyAction::ComponentIdleState;
  auto Error = validateDriverPowerPolicyEvent(Event);
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  Event.Component = 0;
  Event.State = 0;
  Error = validateDriverPowerPolicyEvent(Event);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Event.Action = DriverPowerPolicyAction::PowerNotRequired;
  Error = validateDriverPowerPolicyEvent(Event);
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  Event.Component.reset();
  Event.State.reset();
  Error = validateDriverPowerPolicyEvent(Event);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Event.Action = static_cast<DriverPowerPolicyAction>(UINT32_MAX);
  Error = validateDriverPowerPolicyEvent(Event);
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
}
} // namespace
} // namespace neverd::emulation
