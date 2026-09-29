//===- KernelInterruptActivityTests.cpp - Retained ISR registrations ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify retained interrupt connections and hardware wake ownership.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "os/windows/KernelInterrupts.h"

namespace neverd::emulation {
namespace {
class KernelInterruptActivity : public ::testing::Test {
protected:
  static constexpr uint64_t PDO = 0x2000;
  static constexpr uint64_t PeerPDO = 0x3000;
  static constexpr uint64_t Object = 0x100000;
  static constexpr uint64_t PeerObject = 0x100100;
  static constexpr uint32_t Vector = 0x91;
  static constexpr uint8_t IRQL = 5;
  DriverResult Result;
  bool WakeReady = false;
  unsigned WakeRequests = 0;
  KernelResources Resources{[](uint64_t) { return llvm::Error::success(); }};
  KernelInterrupts Interrupts{Resources, Result};

  void success(llvm::Error Error) {
    if (Error)
      ADD_FAILURE() << llvm::toString(std::move(Error));
  }
  template <typename T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  void reject(llvm::Error Error, llvm::StringRef Text) {
    ASSERT_TRUE(bool(Error));
    const auto Message = llvm::toString(std::move(Error));
    EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
  }
  void add(uint64_t Owner, uint64_t Token, llvm::StringRef ID,
           bool Wake = false,
           DriverInterruptMode Mode = DriverInterruptMode::Latched) {
    DriverPnpDevice Device;
    Device.ID = ID.str();
    Device.Bus = DriverBusKind::RegisterBank;
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    DriverInterruptResource Resource;
    Resource.ID = "line";
    Resource.RawVector = Vector;
    Resource.RawLevel = IRQL;
    Resource.RawAffinity = 1;
    Resource.TranslatedVector = Wake ? Vector + 1 : Vector;
    Resource.TranslatedLevel = IRQL;
    Resource.TranslatedAffinity = 1;
    Resource.Share = DriverInterruptShare::Shared;
    Resource.WakeCapable = Wake;
    Resource.Mode = Mode;
    if (Mode == DriverInterruptMode::LevelSensitive)
      Resource.RetriggerAfter100ns = 3;
    Device.Interrupts.push_back(Resource);
    success(Resources.configure(Owner, Device));
    success(Resources.beginStart(Owner));
    success(Resources.completeLowerStart(Owner, 0));
    success(Resources.finishPnp(Owner, DevicePnpRequest::Start, 0));
    auto Connection =
        take(Interrupts.match(Owner, Resource.TranslatedVector, IRQL, 1));
    Connection.Object = Token;
    Connection.Routine = 0x180001000 + Token;
    if (Wake) {
      Connection.WakeCapable = true;
      Connection.Passive = true;
      Connection.SynchronizeIRQL = 0;
      Connection.Version = interrupts::FullySpecified;
    }
    success(Interrupts.connect(Connection));
  }
  void SetUp() override {
    KernelInterrupts::WakeHost Host;
    Host.Request = [this](uint64_t Owner) {
      EXPECT_EQ(Owner, PeerPDO);
      ++WakeRequests;
      return llvm::Error::success();
    };
    Host.Ready = [this](uint64_t Owner) {
      EXPECT_EQ(Owner, PeerPDO);
      return WakeReady;
    };
    Interrupts.setWakeHost(std::move(Host));
    add(PDO, Object, "device");
  }
  void arm() {
    success(Interrupts.arm({DriverInterruptEvent{0, "device", "line"}}, 0, 0));
  }
};

TEST_F(KernelInterruptActivity, ReactivationRetainsRegistrationAndDeliversISR) {
  success(Interrupts.setActive(Object, false));
  ASSERT_TRUE(Interrupts.connection(Object));
  EXPECT_FALSE(Interrupts.connection(Object)->Active);
  success(Interrupts.setActive(Object, false));
  auto Synchronized = take(Interrupts.synchronize(Object, 0x180003000, 0));
  EXPECT_EQ(take(Interrupts.beginCall(Synchronized.Token.ID, 0, 0)), IRQL);
  take(Interrupts.finishCall(Synchronized.Token.ID, 0, IRQL, 0));
  success(Interrupts.setActive(Object, true));
  success(Interrupts.setActive(Object, true));
  arm();
  auto Delivery = take(Interrupts.queueNextDue(0));
  ASSERT_TRUE(Delivery);
  EXPECT_EQ(Delivery->Call.Arguments.front(), Object);
  EXPECT_EQ(take(Interrupts.beginCall(Delivery->Call.Token.ID, 0, 0)), IRQL);
  take(Interrupts.finishCall(Delivery->Call.Token.ID, 1, IRQL, 0));
  success(Interrupts.setActive(Object, false));
  success(Interrupts.disconnectConnection(Object));
  EXPECT_FALSE(Interrupts.connection(Object));
  reject(Interrupts.setActive(Object, true), "live connection");
}

TEST_F(KernelInterruptActivity, InactiveSourceCannotFabricateDelivery) {
  arm();
  success(Interrupts.setActive(Object, false));
  auto Delivery = Interrupts.queueNextDue(0);
  ASSERT_FALSE(Delivery);
  reject(Delivery.takeError(), "inactive");
  ASSERT_EQ(Result.Interrupts.size(), 1u);
  EXPECT_TRUE(Result.Interrupts.front().Handlers.empty());
  EXPECT_TRUE(Result.Interrupts.front().UndeliveredReason);
  EXPECT_FALSE(Interrupts.connection(Object)->Active);
}

TEST_F(KernelInterruptActivity, SharedDeliverySkipsInactivePeerAndItsDxState) {
  add(PeerPDO, PeerObject, "peer");
  arm();
  success(Interrupts.setActive(PeerObject, false));
  Resources.setPhysicalPower(PeerPDO, DevicePowerState::D3);
  auto Delivery = take(Interrupts.queueNextDue(0));
  ASSERT_TRUE(Delivery);
  EXPECT_EQ(Delivery->Call.Arguments.front(), Object);
  take(Interrupts.beginCall(Delivery->Call.Token.ID, 0, 0));
  auto Returned =
      take(Interrupts.finishCall(Delivery->Call.Token.ID, 0, IRQL, 0));
  EXPECT_FALSE(Returned.Next);
  ASSERT_EQ(Result.Interrupts.front().Handlers.size(), 1u);
  EXPECT_EQ(Result.Interrupts.front().Handlers.front().InterruptObject, Object);
}

TEST_F(KernelInterruptActivity, OwnedCallbacksPreventPartialDeactivation) {
  arm();
  auto Delivery = take(Interrupts.queueNextDue(0));
  ASSERT_TRUE(Delivery);
  reject(Interrupts.setActive(Object, false), "owned");
  EXPECT_TRUE(Interrupts.connection(Object)->Active);
  take(Interrupts.beginCall(Delivery->Call.Token.ID, 0, 0));
  reject(Interrupts.setActive(Object, false), "owned");
  EXPECT_TRUE(Interrupts.connection(Object)->Active);
  take(Interrupts.finishCall(Delivery->Call.Token.ID, 1, IRQL, 0));
  success(Interrupts.setActive(Object, false));
}
TEST_F(KernelInterruptActivity, WakePulseWaitsForPhysicalPowerAndD0Entry) {
  add(PeerPDO, PeerObject, "wake", true);
  Resources.setPhysicalPower(PeerPDO, DevicePowerState::D3);
  success(Interrupts.arm({DriverInterruptEvent{0, "wake", "line"}}, 0, 0));
  EXPECT_EQ(take(Interrupts.dueCount(0)), 0u);
  EXPECT_FALSE(take(Interrupts.queueNextDue(0)));
  EXPECT_EQ(WakeRequests, 1u);
  EXPECT_TRUE(Interrupts.hasPendingWake(PeerPDO));
  EXPECT_FALSE(Interrupts.nextEventTime());
  ASSERT_EQ(Result.Interrupts.size(), 1u);
  EXPECT_EQ(Result.Interrupts.front().OccurredAt100ns,
            std::optional<uint64_t>{0});
  EXPECT_FALSE(Result.Interrupts.front().DeliveredAt100ns);
  Resources.setPhysicalPower(PeerPDO, DevicePowerState::D0);
  EXPECT_FALSE(take(Interrupts.queueNextDue(1)));
  EXPECT_EQ(WakeRequests, 1u);
  EXPECT_TRUE(Interrupts.hasPendingWake(PeerPDO));
  WakeReady = true;
  EXPECT_EQ(take(Interrupts.dueCount(2)), 1u);
  auto Delivery = take(Interrupts.queueNextDue(2));
  ASSERT_TRUE(Delivery);
  EXPECT_EQ(Delivery->IRQL, 0u);
  EXPECT_EQ(Delivery->Call.Arguments.front(), PeerObject);
  EXPECT_EQ(take(Interrupts.beginCall(Delivery->Call.Token.ID, 0, 2)), 0u);
  EXPECT_TRUE(Interrupts.hasPendingWake(PeerPDO));
  take(Interrupts.finishCall(Delivery->Call.Token.ID, 1, 0, 3));
  EXPECT_FALSE(Interrupts.hasPendingWake(PeerPDO));
  EXPECT_FALSE(Interrupts.hasPendingEvents());
  EXPECT_EQ(Result.Interrupts.front().DeliveredAt100ns,
            std::optional<uint64_t>{2});
  EXPECT_EQ(Result.Interrupts.front().ReturnedAt100ns,
            std::optional<uint64_t>{3});
}

TEST_F(KernelInterruptActivity,
       WakeCapableInterruptServicesD0WithoutStartingPowerTransition) {
  add(PeerPDO, PeerObject, "wake", true);
  success(Interrupts.arm({DriverInterruptEvent{0, "wake", "line"}}, 0, 0));
  EXPECT_EQ(take(Interrupts.dueCount(0)), 1u);
  auto Delivery = take(Interrupts.queueNextDue(0));
  ASSERT_TRUE(Delivery);
  EXPECT_EQ(Delivery->IRQL, 0u);
  EXPECT_EQ(WakeRequests, 0u);
  EXPECT_FALSE(Interrupts.hasPendingWake(PeerPDO));
  take(Interrupts.beginCall(Delivery->Call.Token.ID, 0, 0));
  take(Interrupts.finishCall(Delivery->Call.Token.ID, 1, 0, 0));
  EXPECT_FALSE(Interrupts.hasPendingEvents());
}

TEST_F(KernelInterruptActivity,
       WakeBurstSharesPowerRequestAndRetainsEveryPulse) {
  add(PeerPDO, PeerObject, "wake", true);
  Resources.setPhysicalPower(PeerPDO, DevicePowerState::D3);
  const DriverInterruptEvent Event{0, "wake", "line"};
  success(Interrupts.arm({Event, Event}, 0, 0));
  EXPECT_FALSE(take(Interrupts.queueNextDue(0)));
  EXPECT_EQ(WakeRequests, 1u);
  Resources.setPhysicalPower(PeerPDO, DevicePowerState::D0);
  WakeReady = true;
  for (unsigned I = 0; I != 2; ++I) {
    auto Delivery = take(Interrupts.queueNextDue(1));
    ASSERT_TRUE(Delivery);
    EXPECT_FALSE(take(Interrupts.queueNextDue(1)));
    take(Interrupts.beginCall(Delivery->Call.Token.ID, 0, 1));
    EXPECT_TRUE(Interrupts.hasPendingWake(PeerPDO));
    take(Interrupts.finishCall(Delivery->Call.Token.ID, 1, 0, 1));
    EXPECT_EQ(Interrupts.hasPendingWake(PeerPDO), I == 0);
  }
  ASSERT_EQ(Result.Interrupts.size(), 2u);
  for (const auto &Observation : Result.Interrupts)
    EXPECT_EQ(Observation.Handlers.size(), 1u);
}

TEST_F(KernelInterruptActivity, FailedWakeCanDisconnectWithoutCallingItsISR) {
  add(PeerPDO, PeerObject, "wake", true);
  Resources.setPhysicalPower(PeerPDO, DevicePowerState::D3);
  success(Interrupts.arm({DriverInterruptEvent{0, "wake", "line"}}, 0, 0));
  EXPECT_FALSE(take(Interrupts.queueNextDue(0)));
  reject(Interrupts.setActive(PeerObject, false), "latched wake");
  EXPECT_TRUE(Interrupts.connection(PeerObject)->Active);
  success(Interrupts.disconnectConnection(PeerObject));
  EXPECT_FALSE(Interrupts.hasPendingWake(PeerPDO));
  EXPECT_FALSE(Interrupts.hasPendingEvents());
  ASSERT_EQ(Result.Interrupts.size(), 1u);
  EXPECT_TRUE(Result.Interrupts.front().UndeliveredReason);
  EXPECT_TRUE(Result.Interrupts.front().Handlers.empty());
}

TEST_F(KernelInterruptActivity, WakeReadyCannotSubstituteForPhysicalD0) {
  add(PeerPDO, PeerObject, "wake", true);
  Resources.setPhysicalPower(PeerPDO, DevicePowerState::D3);
  success(Interrupts.arm({DriverInterruptEvent{0, "wake", "line"}}, 0, 0));
  EXPECT_FALSE(take(Interrupts.queueNextDue(0)));
  WakeReady = true;
  auto Delivery = Interrupts.queueNextDue(1);
  ASSERT_FALSE(Delivery);
  reject(Delivery.takeError(), "actual device power D0");
  EXPECT_TRUE(Result.Interrupts.front().Handlers.empty());
}

TEST_F(KernelInterruptActivity, DeassertionBeforeD0PreservesLatchedWakeISR) {
  add(PeerPDO, PeerObject, "wake", true, DriverInterruptMode::LevelSensitive);
  Resources.setPhysicalPower(PeerPDO, DevicePowerState::D3);
  DriverInterruptEvent Assert{0, "wake", "line"};
  Assert.Action = DriverInterruptAction::Assert;
  DriverInterruptEvent Deassert{1, "wake", "line"};
  Deassert.Action = DriverInterruptAction::Deassert;
  success(Interrupts.arm({Assert, Deassert}, 0, 0));
  EXPECT_FALSE(take(Interrupts.queueNextDue(0)));
  EXPECT_TRUE(Interrupts.hasPendingWake(PeerPDO));
  EXPECT_EQ(WakeRequests, 1u);
  EXPECT_FALSE(take(Interrupts.queueNextDue(1)));
  EXPECT_TRUE(Interrupts.hasPendingWake(PeerPDO));
  EXPECT_FALSE(Interrupts.nextEventTime());
  Resources.setPhysicalPower(PeerPDO, DevicePowerState::D0);
  WakeReady = true;
  auto Delivery = take(Interrupts.queueNextDue(2));
  ASSERT_TRUE(Delivery);
  take(Interrupts.beginCall(Delivery->Call.Token.ID, 0, 2));
  take(Interrupts.finishCall(Delivery->Call.Token.ID, 1, 0, 2));
  EXPECT_FALSE(Interrupts.hasPendingEvents());
  ASSERT_EQ(Result.Interrupts.size(), 2u);
  EXPECT_EQ(Result.Interrupts.front().Handlers.size(), 1u);
  EXPECT_TRUE(Result.Interrupts.back().Handlers.empty());
}

TEST_F(KernelInterruptActivity, AssertedWakeLineResumesSamplingAfterISRReturn) {
  add(PeerPDO, PeerObject, "wake", true, DriverInterruptMode::LevelSensitive);
  Resources.setPhysicalPower(PeerPDO, DevicePowerState::D3);
  DriverInterruptEvent Assert{0, "wake", "line"};
  Assert.Action = DriverInterruptAction::Assert;
  success(Interrupts.arm({Assert}, 0, 0));
  EXPECT_FALSE(take(Interrupts.queueNextDue(0)));
  Resources.setPhysicalPower(PeerPDO, DevicePowerState::D0);
  WakeReady = true;
  auto Delivery = take(Interrupts.queueNextDue(2));
  ASSERT_TRUE(Delivery);
  take(Interrupts.beginCall(Delivery->Call.Token.ID, 0, 2));
  take(Interrupts.finishCall(Delivery->Call.Token.ID, 1, 0, 2));
  EXPECT_FALSE(Interrupts.hasPendingWake(PeerPDO));
  EXPECT_EQ(Interrupts.nextEventTime(), std::optional<uint64_t>{5});
  EXPECT_FALSE(take(Interrupts.queueNextDue(4)));
  Delivery = take(Interrupts.queueNextDue(5));
  ASSERT_TRUE(Delivery);
  take(Interrupts.beginCall(Delivery->Call.Token.ID, 0, 5));
  DriverInterruptEvent Deassert{0, "wake", "line"};
  Deassert.Action = DriverInterruptAction::Deassert;
  success(Interrupts.arm({Deassert}, 1, 5));
  EXPECT_FALSE(take(Interrupts.queueNextDue(5)));
  take(Interrupts.finishCall(Delivery->Call.Token.ID, 1, 0, 5));
  EXPECT_FALSE(Interrupts.hasPendingEvents());
  EXPECT_EQ(WakeRequests, 1u);
  EXPECT_EQ(Result.Interrupts.front().Handlers.size(), 2u);
}

} // namespace
} // namespace neverd::emulation
