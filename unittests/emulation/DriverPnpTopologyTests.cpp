//===- DriverPnpTopologyTests.cpp - Explicit provider parent topology -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validate one provider parent graph for native options and JSON scenarios.
///
//===----------------------------------------------------------------------===//

#include "DriverScenario.h"
#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::emulation {
namespace {
DriverPnpDevice device(llvm::StringRef ID,
                       std::optional<std::string> Parent = std::nullopt) {
  DriverPnpDevice Device;
  Device.ID = ID.str();
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  Device.ParentID = std::move(Parent);
  return Device;
}

std::string scenario(const DriverOptions &Options) {
  llvm::json::Array Devices;
  for (const auto &Device : Options.PnpDevices) {
    llvm::json::Object Item{{"id", Device.ID},
                            {"bus", "resource_free"},
                            {"initial_device_power", "D0"},
                            {"initial_system_power", "working"}};
    if (Device.ParentID)
      Item["parent_id"] = *Device.ParentID;
    Devices.push_back(std::move(Item));
  }
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  Stream << llvm::json::Value(
      llvm::json::Object{{"pnp_devices", std::move(Devices)}});
  return Text;
}

void rejectsBoth(const DriverOptions &Options, llvm::StringRef Message) {
  auto Error = validateDriverScenario(Options);
  ASSERT_TRUE(bool(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find(Message.str()),
            std::string::npos);
  auto Parsed = driverOptionsFromScenarioJSON(scenario(Options));
  ASSERT_FALSE(bool(Parsed));
  EXPECT_NE(llvm::toString(Parsed.takeError()).find(Message.str()),
            std::string::npos);
}

TEST(DriverPnpTopology,
     ParentOrderAndIndependentRootsPreserveNativeJSONParity) {
  DriverOptions Input;
  Input.PnpDevices = {device("grandchild", "child"), device("child", "root"),
                      device("root"), device("independent")};
  auto Error = validateDriverScenario(Input);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  auto Parsed = driverOptionsFromScenarioJSON(scenario(Input));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_EQ(Parsed->PnpDevices.size(), Input.PnpDevices.size());
  for (size_t I = 0; I != Input.PnpDevices.size(); ++I) {
    EXPECT_EQ(Parsed->PnpDevices[I].ID, Input.PnpDevices[I].ID);
    EXPECT_EQ(Parsed->PnpDevices[I].ParentID, Input.PnpDevices[I].ParentID);
  }
}

TEST(DriverPnpTopology, RejectsUnknownSelfAndCyclicParentsBeforeExecution) {
  DriverOptions Input;
  Input.PnpDevices = {device("child", "missing")};
  rejectsBoth(Input, "configured device");
  Input.PnpDevices = {device("child", "child")};
  rejectsBoth(Input, "itself");
  Input.PnpDevices = {device("first", "second"), device("second", "first")};
  rejectsBoth(Input, "cycle");
  Input.PnpDevices = {device("first", "second"), device("second", "third"),
                      device("third", "first"), device("root")};
  rejectsBoth(Input, "cycle");
  Input.PnpDevices = {device("child", "ROOT"), device("root")};
  rejectsBoth(Input, "configured device");
}

TEST(DriverPnpTopology, ParentIdentifiersShareTheBoundedDeviceIDContract) {
  for (const std::string Parent :
       {std::string(), std::string("with space"), std::string("/parent"),
        std::string(DriverScenarioDeviceIDLimit + 1, 'p')}) {
    SCOPED_TRACE(Parent);
    DriverOptions Input;
    Input.PnpDevices = {device("child", Parent)};
    rejectsBoth(Input, "bounded ASCII identifier");
  }
}

TEST(DriverPnpTopology, PresentParentFieldRequiresAStringAndDoesNotInferARoot) {
  for (const auto Parent : {"null", "false", "1", "[]", "{}"}) {
    SCOPED_TRACE(Parent);
    const std::string Text =
        R"({"pnp_devices":[{"id":"child","bus":"resource_free",
        "initial_device_power":"D0","initial_system_power":"working",
        "parent_id":)" +
        std::string(Parent) + "}]}";
    auto Parsed = driverOptionsFromScenarioJSON(Text);
    ASSERT_FALSE(bool(Parsed));
    EXPECT_NE(
        llvm::toString(Parsed.takeError()).find("parent_id must be a string"),
        std::string::npos);
  }
}

TEST(DriverPnpTopology, ReportsConfigurationAndStableObservedParentSeparately) {
  DriverResult Result;
  Result.Configuration.PnpDevices = {device("child", "root"), device("root")};
  DriverPnpDeviceResult Child;
  Child.ID = "child";
  Child.PDO = 0x2000;
  Child.ParentID = "root";
  Child.ParentPDO = 0x1000;
  Result.PnpDevices.push_back(Child);
  DriverPnpDeviceResult Root;
  Root.ID = "root";
  Root.PDO = 0x1000;
  Result.PnpDevices.push_back(Root);
  auto JSON = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
  const auto *Configuration =
      JSON->getAsObject()->getObject("configuration")->getArray("pnp_devices");
  EXPECT_EQ((*Configuration)[0].getAsObject()->getString("parent_id"), "root");
  EXPECT_EQ((*Configuration)[1].getAsObject()->get("parent_id"), nullptr);
  const auto *Observed = JSON->getAsObject()->getArray("pnp_devices");
  EXPECT_EQ((*Observed)[0].getAsObject()->getString("parent_id"), "root");
  EXPECT_EQ((*Observed)[0].getAsObject()->getString("parent_pdo"), "0x1000");
  EXPECT_EQ((*Observed)[1].getAsObject()->get("parent_id")->kind(),
            llvm::json::Value::Null);
  EXPECT_EQ((*Observed)[1].getAsObject()->get("parent_pdo")->kind(),
            llvm::json::Value::Null);
}
} // namespace
} // namespace neverd::emulation
