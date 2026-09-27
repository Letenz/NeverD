//===- DriverUsbIdleScenarioTests.cpp - Explicit USB idle facts ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "DriverScenario.h"
#include "gtest/gtest.h"
#include "windows/KernelUsbIdle.h"
#include "windows/WindowsKernelLayout.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {

std::string scenario(llvm::StringRef Usb,
                     llvm::StringRef Wake = R"({"s0":true,"sx":false})",
                     llvm::StringRef Event = {}) {
  std::string Text =
      R"({"pnp_devices":[{"id":"port","bus":"resource_free",
        "initial_device_power":"D0","initial_system_power":"working",
        "wake_capabilities":)" +
      Wake.str() + R"(,"usb_idle":)" + Usb.str() + "}]";
  if (!Event.empty())
    Text += R"(,"requests":[{"kind":"create","device_id":"port"},
      {"kind":"ioctl","device_id":"port","code":"0x222000",
       "power_policy_events":[)" +
            Event.str() + "]} ]";
  return Text + "}";
}

DriverPnpDevice device(llvm::StringRef ID, DriverUsbIdleRole Role) {
  DriverPnpDevice Device;
  Device.ID = ID.str();
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  Device.UsbIdle = DriverUsbIdleConfig{Role, false};
  return Device;
}

void permission(DriverOptions &Options, llvm::StringRef DeviceID) {
  DriverRequest Create;
  Create.Kind = DriverRequestKind::Create;
  Create.DeviceID = DeviceID.str();
  DriverRequest Observe;
  Observe.Kind = DriverRequestKind::DeviceControl;
  Observe.DeviceID = DeviceID.str();
  Observe.PowerPolicyEvents.push_back(
      {0, DeviceID.str(), DriverPowerPolicyAction::UsbIdlePermission});
  Options.Requests = {Create, Observe};
}

void rejectNativeAndInherited(const DriverOptions &Options,
                              llvm::StringRef Diagnostic) {
  auto Error = validateDriverScenario(Options);
  ASSERT_TRUE(bool(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find(Diagnostic.str()),
            std::string::npos);
  auto Parsed = driverOptionsFromScenarioJSON("{}", Options);
  ASSERT_FALSE(bool(Parsed));
  EXPECT_NE(llvm::toString(Parsed.takeError()).find(Diagnostic.str()),
            std::string::npos);
}

TEST(DriverUsbIdleScenario,
     ExplicitRoleDoesNotInferRemoteWakeFromGenericFacts) {
  auto Options = driverOptionsFromScenarioJSON(
      scenario(R"({"role":"independent_function"})"));
  ASSERT_TRUE(bool(Options)) << llvm::toString(Options.takeError());
  ASSERT_TRUE(Options->PnpDevices[0].UsbIdle);
  EXPECT_EQ(Options->PnpDevices[0].Bus, DriverBusKind::ResourceFree);
  EXPECT_EQ(Options->PnpDevices[0].UsbIdle->Role,
            DriverUsbIdleRole::IndependentFunction);
  EXPECT_FALSE(Options->PnpDevices[0].UsbIdle->RemoteWake);
  EXPECT_TRUE(Options->PnpDevices[0].WakeCapabilities->S0);

  DriverResult Result;
  Result.Configuration = *Options;
  auto JSON = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
  const auto *Devices =
      JSON->getAsObject()->getObject("configuration")->getArray("pnp_devices");
  ASSERT_NE(Devices, nullptr);
  const auto *Usb = Devices->front().getAsObject()->getObject("usb_idle");
  ASSERT_NE(Usb, nullptr);
  EXPECT_EQ(Usb->getString("role"), "independent_function");
  EXPECT_EQ(Usb->getBoolean("remote_wake"), false);
}

TEST(DriverUsbIdleScenario, DeviceWakeIsAnExplicitIndependentBusFact) {
  for (const bool RemoteWake : {false, true}) {
    const auto Usb =
        std::string(
            R"({"role":"independent_function","device_wake":"D2","remote_wake":)") +
        (RemoteWake ? "true}" : "false}");
    auto Options = driverOptionsFromScenarioJSON(scenario(Usb));
    ASSERT_TRUE(bool(Options)) << llvm::toString(Options.takeError());
    const auto &Config = *Options->PnpDevices.front().UsbIdle;
    EXPECT_EQ(Config.DeviceWake, DevicePowerState::D2);
    EXPECT_EQ(Config.RemoteWake, RemoteWake);
    DriverResult Result;
    Result.Configuration = *Options;
    auto JSON = llvm::json::parse(driverResultJSON(Result));
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
    const auto *Reported = JSON->getAsObject()
                               ->getObject("configuration")
                               ->getArray("pnp_devices")
                               ->front()
                               .getAsObject()
                               ->getObject("usb_idle");
    ASSERT_NE(Reported, nullptr);
    EXPECT_EQ(Reported->getString("device_wake"), "D2");
    EXPECT_EQ(Reported->getBoolean("remote_wake"), RemoteWake);
  }
  auto Unknown = driverOptionsFromScenarioJSON(
      scenario(R"({"role":"independent_function","remote_wake":true})"));
  ASSERT_TRUE(bool(Unknown)) << llvm::toString(Unknown.takeError());
  EXPECT_FALSE(Unknown->PnpDevices.front().UsbIdle->DeviceWake);
  DriverResult Result;
  Result.Configuration = *Unknown;
  auto JSON = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
  EXPECT_EQ(JSON->getAsObject()
                ->getObject("configuration")
                ->getArray("pnp_devices")
                ->front()
                .getAsObject()
                ->getObject("usb_idle")
                ->get("device_wake"),
            nullptr);
}

TEST(DriverUsbIdleScenario, DeviceWakeRequiresASupportedFunctionCapability) {
  for (const auto *State : {"null", "true", "2", "[]", "{}", "\"D0\"", "\"D1\"",
                            "\"D3\"", "\"D2hot\"", "\"d2\"", "\"maximum\""}) {
    SCOPED_TRACE(State);
    auto Parsed = driverOptionsFromScenarioJSON(scenario(
        std::string(R"({"role":"independent_function","device_wake":)") +
        State + "}"));
    ASSERT_FALSE(bool(Parsed));
    EXPECT_NE(llvm::toString(Parsed.takeError()).find("device_wake"),
              std::string::npos);
  }
  DriverOptions Options;
  Options.PnpDevices = {device("parent", DriverUsbIdleRole::CompositeParent),
                        device("child", DriverUsbIdleRole::CompositeFunction)};
  Options.PnpDevices[1].ParentID = "parent";
  Options.PnpDevices[1].UsbIdle->DeviceWake = DevicePowerState::D2;
  auto Error = validateDriverScenario(Options);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  for (auto State :
       {DevicePowerState::D0, DevicePowerState::D1, DevicePowerState::D3,
        static_cast<DevicePowerState>(0), static_cast<DevicePowerState>(5),
        static_cast<DevicePowerState>(UINT32_MAX)}) {
    auto Invalid = Options;
    Invalid.PnpDevices[1].UsbIdle->DeviceWake = State;
    rejectNativeAndInherited(Invalid, "device_wake");
  }
  Options.PnpDevices[0].UsbIdle->DeviceWake = DevicePowerState::D2;
  rejectNativeAndInherited(Options, "device_wake");
}

TEST(DriverUsbIdleScenario, MissingRoleCapabilityKeepsAnOrdinaryProvider) {
  DriverOptions Options;
  auto Device = device("ordinary", DriverUsbIdleRole::IndependentFunction);
  Device.UsbIdle.reset();
  Device.WakeCapabilities = DriverWakeCapabilities{true, true};
  Options.PnpDevices.push_back(Device);
  auto Parsed = driverOptionsFromScenarioJSON("{}", Options);
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_FALSE(Parsed->PnpDevices[0].UsbIdle);
  permission(Options, "ordinary");
  rejectNativeAndInherited(Options, "usb_idle_permission");
}

TEST(DriverUsbIdleScenario, MalformedAndUnknownConfigurationFailsBeforeLoad) {
  for (const char *Usb :
       {"null", "false", "[]", "{}", R"({"role":1})", R"({"role":"hub"})",
        R"({"role":"IndependentFunction"})",
        R"({"role":"independent_function","remote_wake":null})",
        R"({"role":"independent_function","remote_wake":1})",
        R"({"role":"independent_function","d2":true})"}) {
    SCOPED_TRACE(Usb);
    auto Options = driverOptionsFromScenarioJSON(scenario(Usb));
    ASSERT_FALSE(bool(Options));
    EXPECT_FALSE(llvm::toString(Options.takeError()).empty());
  }
  DriverOptions Options;
  Options.PnpDevices.push_back(
      device("port", static_cast<DriverUsbIdleRole>(UINT32_MAX)));
  rejectNativeAndInherited(Options, "unsupported usb_idle role");
  auto Executed = emulateDriver("missing-usb-schema-image.sys", Options);
  ASSERT_FALSE(bool(Executed));
  EXPECT_NE(llvm::toString(Executed.takeError()).find("usb_idle role"),
            std::string::npos);
}

TEST(DriverUsbIdleScenario, RemoteWakeRequiresAnExplicitConsistentDeclaration) {
  auto Parsed = driverOptionsFromScenarioJSON(
      scenario(R"({"role":"independent_function","remote_wake":true})"));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_TRUE(Parsed->PnpDevices[0].UsbIdle->RemoteWake);
  Parsed->PnpDevices[0].WakeCapabilities.reset();
  rejectNativeAndInherited(*Parsed, "wake_capabilities");
  Parsed->PnpDevices[0].WakeCapabilities = DriverWakeCapabilities{false, false};
  rejectNativeAndInherited(*Parsed, "wake_capabilities");
  Parsed->PnpDevices[0].UsbIdle->RemoteWake = false;
  auto Error = validateDriverScenario(*Parsed);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  auto Contradiction = driverOptionsFromScenarioJSON(
      scenario(R"({"role":"independent_function","remote_wake":true})",
               R"({"s0":false,"sx":false})"));
  ASSERT_FALSE(bool(Contradiction));
  EXPECT_NE(llvm::toString(Contradiction.takeError()).find("wake_capabilities"),
            std::string::npos);
}

TEST(DriverUsbIdleScenario, CompositeFunctionsRequireAnImmediateTypedParent) {
  DriverOptions Options;
  Options.PnpDevices = {device("parent", DriverUsbIdleRole::CompositeParent),
                        device("child", DriverUsbIdleRole::CompositeFunction)};
  Options.PnpDevices[1].ParentID = "parent";
  auto Error = validateDriverScenario(Options);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));

  auto Invalid = Options;
  Invalid.PnpDevices[1].ParentID.reset();
  rejectNativeAndInherited(Invalid, "immediate composite_parent");
  Invalid = Options;
  Invalid.PnpDevices[0].UsbIdle.reset();
  rejectNativeAndInherited(Invalid, "immediate composite_parent");
  Invalid = Options;
  Invalid.PnpDevices[0].UsbIdle->Role = DriverUsbIdleRole::IndependentFunction;
  rejectNativeAndInherited(Invalid, "immediate composite_parent");
  Invalid = Options;
  Invalid.PnpDevices[1].UsbIdle->Role = DriverUsbIdleRole::IndependentFunction;
  rejectNativeAndInherited(Invalid, "composite_function role");
  Invalid = Options;
  Invalid.PnpDevices[0].UsbIdle->RemoteWake = true;
  Invalid.PnpDevices[0].WakeCapabilities = DriverWakeCapabilities{true, true};
  rejectNativeAndInherited(Invalid, "composite_parent cannot");
}

TEST(DriverUsbIdleScenario, PermissionTargetsTheWholeExplicitCoordinator) {
  DriverOptions Options;
  Options.PnpDevices = {device("parent", DriverUsbIdleRole::CompositeParent),
                        device("child", DriverUsbIdleRole::CompositeFunction),
                        device("port", DriverUsbIdleRole::IndependentFunction)};
  Options.PnpDevices[1].ParentID = "parent";
  for (const auto *Target : {"parent", "port"}) {
    permission(Options, Target);
    auto Error = validateDriverScenario(Options);
    EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  }
  permission(Options, "child");
  rejectNativeAndInherited(Options, "usb_idle_permission");
  permission(Options, "parent");
  Options.PnpDevices.erase(Options.PnpDevices.begin() + 1);
  rejectNativeAndInherited(Options, "configured function children");
}

TEST(DriverUsbIdleScenario, PermissionCannotSmuggleAComponentOrPowerState) {
  constexpr llvm::StringLiteral Usb = R"({"role":"independent_function"})";
  constexpr llvm::StringLiteral Event =
      R"({"after_100ns":0,"device_id":"port","action":"usb_idle_permission"})";
  auto Parsed = driverOptionsFromScenarioJSON(
      scenario(Usb, R"({"s0":false,"sx":false})", Event));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  auto &Decision = Parsed->Requests[1].PowerPolicyEvents[0];
  EXPECT_EQ(Decision.Action, DriverPowerPolicyAction::UsbIdlePermission);
  EXPECT_FALSE(Decision.Component);
  EXPECT_FALSE(Decision.State);
  Decision.Component = 0;
  rejectNativeAndInherited(*Parsed, "component_idle_state");
  Decision.Component.reset();
  Decision.State = 0;
  rejectNativeAndInherited(*Parsed, "component_idle_state");
  for (const auto *Field : {"component", "state"}) {
    std::string Invalid = Event.str();
    Invalid.pop_back();
    Invalid += std::string(",\"") + Field + "\":0}";
    auto Result = driverOptionsFromScenarioJSON(
        scenario(Usb, R"({"s0":true,"sx":false})", Invalid));
    ASSERT_FALSE(bool(Result));
    EXPECT_NE(llvm::toString(Result.takeError()).find("component_idle_state"),
              std::string::npos);
  }
}

TEST(DriverUsbIdleScenario,
     ReportsExactCapturedMembersWithoutInventingProgress) {
  DriverResult Result;
  DriverPowerPolicyResult Event;
  Event.DeviceID = "parent";
  Event.Action = DriverPowerPolicyAction::UsbIdlePermission;
  Event.DueAt100ns = 42;
  Event.UsbIdleMembers = {{"child", 0xfffff80000001000, 0xfffff80000002000, 7}};
  Result.PowerPolicyEvents.push_back(Event);
  DriverRequestResult Request;
  Request.Kind = DriverRequestKind::InternalDeviceControl;
  Request.Origin = DriverRequestOrigin::DriverAllocatedIRP;
  Request.DeviceID = "child";
  Request.IRP = Event.UsbIdleMembers[0].IRP;
  Request.UsbIdle = DriverUsbIdleRequestResult{};
  Request.UsbIdle->StartEpoch = 7;
  Request.UsbIdle->BusReceivedAt100ns = 0;
  Result.Requests.push_back(Request);
  auto JSON = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
  const auto *Root = JSON->getAsObject();
  const auto *ObservedEvent =
      Root->getArray("power_policy_events")->front().getAsObject();
  EXPECT_EQ(ObservedEvent->getString("action"), "usb_idle_permission");
  EXPECT_EQ(ObservedEvent->get("occurred_at_100ns")->kind(),
            llvm::json::Value::Null);
  const auto *Members = ObservedEvent->getArray("usb_idle_members");
  ASSERT_NE(Members, nullptr);
  ASSERT_EQ(Members->size(), 1u);
  const auto *Member = Members->front().getAsObject();
  EXPECT_EQ(Member->getString("device_id"), "child");
  EXPECT_EQ(Member->getString("pdo"), "0xFFFFF80000001000");
  EXPECT_EQ(Member->getString("irp"), "0xFFFFF80000002000");
  EXPECT_EQ(Member->getInteger("start_epoch"), 7);
  const auto *Observed = Root->getArray("requests")->front().getAsObject();
  EXPECT_EQ(Observed->getBoolean("completed"), false);
  EXPECT_EQ(Observed->get("file")->kind(), llvm::json::Value::Null);
  const auto *Usb = Observed->getObject("usb_idle");
  ASSERT_NE(Usb, nullptr);
  EXPECT_EQ(Usb->getInteger("start_epoch"), 7);
  EXPECT_EQ(Usb->getInteger("bus_received_at_100ns"), 0);
  for (const auto *Field :
       {"callback_entered_at_100ns", "callback_returned_at_100ns", "d2_irp",
        "d2_status", "d2_completed_at_100ns", "completion_cause",
        "completion_claimed_at_100ns", "completed_at_100ns"}) {
    SCOPED_TRACE(Field);
    ASSERT_NE(Usb->get(Field), nullptr);
    EXPECT_EQ(Usb->get(Field)->kind(), llvm::json::Value::Null);
  }
}

TEST(DriverUsbIdleScenario, FrameworkOriginHasNoScenarioFileOrResponseSlot) {
  DriverResult Result;
  DriverRequestResult Request;
  Request.Kind = DriverRequestKind::InternalDeviceControl;
  Request.Origin = DriverRequestOrigin::FrameworkUsbIdle;
  Request.ControlCode = usb_idle::SubmitIdleNotification;
  Result.Requests.push_back(Request);
  auto JSON = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
  const auto *Observed =
      JSON->getAsObject()->getArray("requests")->front().getAsObject();
  EXPECT_EQ(Observed->getString("origin"), "framework_usb_idle");
  EXPECT_EQ(Observed->getString("kind"), "internal_ioctl");
  for (const auto *Field : {"file", "response_index", "usb_idle"}) {
    ASSERT_NE(Observed->get(Field), nullptr);
    EXPECT_EQ(Observed->get(Field)->kind(), llvm::json::Value::Null);
  }
}

TEST(DriverUsbIdleScenario, ReportsD2AndWinningCompletionAsIndependentFacts) {
  DriverResult Result;
  DriverRequestResult Request;
  Request.Kind = DriverRequestKind::InternalDeviceControl;
  Request.Origin = DriverRequestOrigin::DriverAllocatedIRP;
  Request.Completed = true;
  Request.IOStatus = 0;
  DriverUsbIdleRequestResult Usb;
  Usb.StartEpoch = 2;
  Usb.BusReceivedAt100ns = 1;
  Usb.CallbackEnteredAt100ns = 3;
  Usb.D2IRP = 0xfffff80000003000;
  Usb.D2Status = 0;
  Usb.D2CompletedAt100ns = 5;
  Usb.CallbackReturnedAt100ns = 6;
  Usb.CompletionClaimedAt100ns = 9;
  Usb.CompletedAt100ns = 10;
  const std::pair<DriverUsbIdleCompletionCause, const char *> Causes[] = {
      {DriverUsbIdleCompletionCause::Cancel, "cancel"},
      {DriverUsbIdleCompletionCause::SystemSleep, "system_sleep"},
      {DriverUsbIdleCompletionCause::Remove, "remove"},
      {DriverUsbIdleCompletionCause::DeviceD0, "device_d0"},
      {DriverUsbIdleCompletionCause::DeviceD3, "device_d3"}};
  for (const auto &[Cause, Name] : Causes) {
    Usb.CompletionCause = Cause;
    Request.UsbIdle = Usb;
    Result.Requests = {Request};
    auto JSON = llvm::json::parse(driverResultJSON(Result));
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
    const auto *Observed = JSON->getAsObject()
                               ->getArray("requests")
                               ->front()
                               .getAsObject()
                               ->getObject("usb_idle");
    ASSERT_NE(Observed, nullptr);
    EXPECT_EQ(Observed->getString("completion_cause"), Name);
    EXPECT_EQ(Observed->getInteger("completion_claimed_at_100ns"), 9);
    EXPECT_EQ(Observed->getInteger("completed_at_100ns"), 10);
    EXPECT_EQ(Observed->getString("d2_irp"), "0xFFFFF80000003000");
    EXPECT_EQ(Observed->getInteger("d2_status"), 0);
    EXPECT_EQ(Observed->getInteger("d2_completed_at_100ns"), 5);
    EXPECT_EQ(Observed->getInteger("callback_entered_at_100ns"), 3);
    EXPECT_EQ(Observed->getInteger("callback_returned_at_100ns"), 6);
  }
}

TEST(DriverUsbIdleScenario, ExpectedCompletionRequiresExactProtocolEvidence) {
  DriverResult Result;
  Result.Stop = DriverStopReason::Returned;
  Result.NTStatus = windows::StatusSuccess;
  DriverRequestResult Request;
  Request.Kind = DriverRequestKind::InternalDeviceControl;
  Request.Origin = DriverRequestOrigin::DriverAllocatedIRP;
  Request.ControlCode = usb_idle::SubmitIdleNotification;
  Request.Completed = true;
  Request.DispatchStatus = windows::StatusPending;
  Request.UsbIdle = DriverUsbIdleRequestResult{};
  Request.UsbIdle->StartEpoch = 1;
  Request.UsbIdle->BusReceivedAt100ns = 0;
  const auto Succeeded = [&](const DriverRequestResult &Value) {
    Result.Requests = {Value};
    auto JSON = llvm::json::parse(driverResultJSON(Result));
    EXPECT_TRUE(bool(JSON));
    if (!JSON) {
      llvm::consumeError(JSON.takeError());
      return false;
    }
    return JSON->getAsObject()->getBoolean("scenario_success").value_or(false);
  };
  for (auto Origin : {DriverRequestOrigin::DriverAllocatedIRP,
                      DriverRequestOrigin::FrameworkUsbIdle}) {
    Request.Origin = Origin;
    for (auto Cause : {DriverUsbIdleCompletionCause::Cancel,
                       DriverUsbIdleCompletionCause::SystemSleep,
                       DriverUsbIdleCompletionCause::Remove,
                       DriverUsbIdleCompletionCause::DeviceD3}) {
      Request.UsbIdle->CompletionCause = Cause;
      Request.IOStatus = Cause == DriverUsbIdleCompletionCause::DeviceD3
                             ? usb_idle::StatusPowerStateInvalid
                             : windows::StatusCancelled;
      EXPECT_TRUE(Succeeded(Request));
      auto Missing = Request;
      Missing.UsbIdle->BusReceivedAt100ns.reset();
      EXPECT_FALSE(Succeeded(Missing));
      Missing = Request;
      Missing.UsbIdle->CompletionCause.reset();
      EXPECT_FALSE(Succeeded(Missing));
      Missing = Request;
      Missing.Origin = DriverRequestOrigin::PoRequestPowerIrp;
      EXPECT_FALSE(Succeeded(Missing));
      Missing = Request;
      Missing.Kind = DriverRequestKind::DeviceControl;
      EXPECT_FALSE(Succeeded(Missing));
      Missing = Request;
      ++Missing.ControlCode;
      EXPECT_FALSE(Succeeded(Missing));
      Missing = Request;
      Missing.IOStatus = windows::StatusDeviceBusy;
      EXPECT_FALSE(Succeeded(Missing));
      Missing.IOStatus = windows::StatusSuccess;
      EXPECT_TRUE(Succeeded(Missing));
      Missing = Request;
      Missing.UsbIdle->CompletionCause = DriverUsbIdleCompletionCause::DeviceD0;
      EXPECT_FALSE(Succeeded(Missing));
    }
  }
}

} // namespace
} // namespace neverd::emulation
