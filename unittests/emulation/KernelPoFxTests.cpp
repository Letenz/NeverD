//===- KernelPoFxTests.cpp - Component power callback and ownership ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise component reference counts, retained transitions and callback
/// completion.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "os/windows/kernel/KernelPoFx.h"

#include <string>
#include <utility>

namespace neverd::emulation {
namespace {
using Kind = KernelPoFx::CallbackKind;

void success(llvm::Error Error) {
  if (Error)
    ADD_FAILURE() << llvm::toString(std::move(Error));
}

template <class T> T take(llvm::Expected<T> Result) {
  if (!Result) {
    ADD_FAILURE() << llvm::toString(Result.takeError());
    return {};
  }
  return std::move(*Result);
}

void expectError(llvm::Error Error, llvm::StringRef Text) {
  ASSERT_TRUE(bool(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find(Text.str()),
            std::string::npos);
}

class DriverKernelPoFx : public ::testing::Test {
protected:
  static constexpr uint64_t Handle = 0x1000;
  static constexpr uint64_t PDO = 0x2000;
  static constexpr uint64_t Context = 0x3000;
  static constexpr uint64_t ActivePC = 0x180001000;
  static constexpr uint64_t IdlePC = 0x180001100;
  static constexpr uint64_t StatePC = 0x180001200;
  static constexpr uint64_t RequiredPC = 0x180001300;
  static constexpr uint64_t NotRequiredPC = 0x180001400;
  KernelPoFx Model;

  KernelPoFx::Registration description(unsigned Components = 1) {
    KernelPoFx::Registration Description;
    Description.PDO = PDO;
    Description.Context = Context;
    Description.Routines = {ActivePC,   IdlePC,        StatePC,
                            RequiredPC, NotRequiredPC, 0};
    Description.Components.resize(Components);
    for (auto &C : Description.Components) {
      C.DeepestWakeableState = 1;
      C.IdleStates = {{0, 0, 100}, {10, 20, 50}, {100, 200, 10}};
    }
    return Description;
  }

  void registerDevice(unsigned Components = 1) {
    success(Model.registerDevice(Handle, description(Components)));
  }

  KernelPoFx::Callback next(Kind Expected, uint32_t Component = 0) {
    auto Call = Model.nextCallback();
    EXPECT_TRUE(Call.has_value());
    if (!Call)
      return {};
    EXPECT_EQ(Call->Kind, Expected);
    EXPECT_EQ(Call->Component, Component);
    EXPECT_EQ(Call->Handle, Handle);
    EXPECT_EQ(Call->PDO, PDO);
    EXPECT_EQ(Call->Arguments.front(), Context);
    success(Model.submitCallback(Call->Token));
    success(Model.beginCallback(Call->Token));
    return *Call;
  }

  void acknowledge(const KernelPoFx::Callback &Call) {
    switch (Call.Kind) {
    case Kind::ActiveCondition:
      return;
    case Kind::IdleCondition:
      success(Model.completeIdleCondition(Call.Handle, Call.Component));
      return;
    case Kind::IdleState:
      success(Model.completeIdleState(Call.Handle, Call.Component));
      return;
    case Kind::DevicePowerRequired:
      success(Model.reportDevicePoweredOn(Call.Handle));
      return;
    case Kind::DevicePowerNotRequired:
      success(Model.completeDevicePowerNotRequired(Call.Handle));
      return;
    }
  }

  void complete(Kind Expected, uint32_t Component = 0) {
    const auto Call = next(Expected, Component);
    acknowledge(Call);
    success(Model.finishCallback(Call.Token));
  }

  void startIdle() {
    registerDevice();
    success(Model.start(Handle));
    complete(Kind::IdleCondition);
    EXPECT_TRUE(take(Model.conditionReached(Handle, 0, false)));
  }
};

TEST_F(DriverKernelPoFx, RegistrationCopiesStateDescriptionsAndRetainsPDO) {
  auto Description = description();
  success(Model.registerDevice(Handle, Description));
  Description.Components[0].IdleStates[1].TransitionLatency = 999;
  EXPECT_EQ(
      Model.registration(Handle)->Components[0].IdleStates[1].TransitionLatency,
      10u);
  EXPECT_EQ(Model.handleForPDO(PDO), Handle);
  expectError(Model.canReleasePDO(PDO), "retained");
  expectError(Model.registerDevice(Handle + 1, Description), "already");
  success(Model.unregisterDevice(Handle));
  success(Model.canReleasePDO(PDO));
  EXPECT_FALSE(Model.registration(Handle));
  EXPECT_FALSE(Model.handleForPDO(PDO));
}

TEST_F(DriverKernelPoFx, RejectsMalformedOrUnsupportedRegistrationAtomically) {
  auto D = description();
  D.Version = pofx::Version1 + 1;
  expectError(Model.registerDevice(Handle, D), "version 1");
  D = description();
  D.Components.clear();
  expectError(Model.registerDevice(Handle, D), "component count");
  D = description();
  D.Components[0].IdleStates.clear();
  expectError(Model.registerDevice(Handle, D), "idle-state count");
  D = description();
  D.Components[0].DeepestWakeableState = 3;
  expectError(Model.registerDevice(Handle, D), "wakeable state");
  D = description();
  D.Components[0].IdleStates[0].TransitionLatency = 1;
  expectError(Model.registerDevice(Handle, D), "F0");
  D = description();
  D.Components[0].IdleStates[0].ResidencyRequirement = 1;
  expectError(Model.registerDevice(Handle, D), "F0");
  D = description();
  D.Routines.IdleState = 0;
  expectError(Model.registerDevice(Handle, D), "all component callbacks");
  D = description();
  D.Routines.DevicePowerRequired = 0;
  expectError(Model.registerDevice(Handle, D), "paired return path");
  D = description();
  D.Routines.PowerControl = StatePC;
  expectError(Model.registerDevice(Handle, D), "explicit provider");
  D = description(2);
  D.Components[0].ID[0] = D.Components[1].ID[0] = 1;
  expectError(Model.registerDevice(Handle, D), "unique");
  EXPECT_FALSE(Model.registration(Handle));
  EXPECT_FALSE(Model.hasPendingCallbacks());
  registerDevice();
}

TEST_F(DriverKernelPoFx, RegistrationBoundsAreAppliedBeforePublication) {
  KernelPoFx::Limits Bounds;
  Bounds.MaxDevices = 1;
  Bounds.MaxComponents = 1;
  Bounds.MaxIdleStates = 2;
  Model = KernelPoFx(Bounds);
  auto D = description(2);
  expectError(Model.registerDevice(Handle, D), "component count");
  D = description();
  expectError(Model.registerDevice(Handle, D), "idle-state count");
  D.Components[0].IdleStates.resize(2);
  success(Model.registerDevice(Handle, D));
  D.PDO += 1;
  expectError(Model.registerDevice(Handle + 1, D), "registration bound");
}

TEST_F(DriverKernelPoFx,
       PreStartActivationPreservesActiveConditionAndReferences) {
  registerDevice(2);
  EXPECT_TRUE(take(Model.component(Handle, 0)).Active);
  success(Model.activate(Handle, 0));
  success(Model.activate(Handle, 0));
  success(Model.start(Handle));
  complete(Kind::IdleCondition, 1);
  EXPECT_FALSE(Model.nextCallback());
  EXPECT_EQ(take(Model.component(Handle, 0)).References, 2u);
  success(Model.idle(Handle, 0));
  EXPECT_FALSE(Model.nextCallback());
  success(Model.idle(Handle, 0));
  complete(Kind::IdleCondition);
  expectError(Model.idle(Handle, 0), "matching activation");
  expectError(Model.start(Handle), "already started");
  EXPECT_EQ(take(Model.component(Handle, 0)).References, 0u);
}

TEST_F(DriverKernelPoFx, CompletionsRequireDeliveredCallbacksAndGuestReturn) {
  registerDevice();
  success(Model.start(Handle));
  const auto Call = *Model.nextCallback();
  expectError(Model.completeIdleCondition(Handle, 0), "delivered");
  expectError(Model.beginCallback(Call.Token), "submitted");
  success(Model.submitCallback(Call.Token));
  expectError(Model.completeIdleCondition(Handle, 0), "delivered");
  expectError(Model.submitCallback(Call.Token), "already");
  success(Model.beginCallback(Call.Token));
  expectError(Model.beginCallback(Call.Token), "one submitted");
  expectError(Model.completeIdleState(Handle, 0), "matching");
  success(Model.completeIdleCondition(Handle, 0));
  expectError(Model.completeIdleCondition(Handle, 0), "matching");
  expectError(Model.unregisterDevice(Handle), "retained");
  EXPECT_TRUE(Model.callback(Call.Token));
  EXPECT_FALSE(take(Model.conditionReached(Handle, 0, false)));
  success(Model.finishCallback(Call.Token));
  EXPECT_FALSE(Model.callback(Call.Token));
  EXPECT_TRUE(take(Model.conditionReached(Handle, 0, false)));
  expectError(Model.finishCallback(Call.Token), "live callback");
  success(Model.unregisterDevice(Handle));
}

TEST_F(DriverKernelPoFx, CompletionAfterReturnKeepsThePendingTransitionAlive) {
  registerDevice();
  success(Model.start(Handle));
  const auto Call = next(Kind::IdleCondition);
  success(Model.finishCallback(Call.Token));
  EXPECT_TRUE(Model.hasPendingCallbacks());
  EXPECT_FALSE(Model.nextCallback());
  EXPECT_FALSE(take(Model.conditionReached(Handle, 0, false)));
  expectError(Model.requestIdleState(Handle, 0, 1), "idle component");
  expectError(Model.canUnregisterDevice(Handle), "retained");
  success(Model.completeIdleCondition(Handle, 0));
  EXPECT_FALSE(Model.hasPendingCallbacks());
  EXPECT_TRUE(take(Model.conditionReached(Handle, 0, false)));
}

TEST_F(DriverKernelPoFx, ActivationRestoresF0BeforeTheActiveNotification) {
  startIdle();
  success(Model.requestIdleState(Handle, 0, 2));
  auto Call = next(Kind::IdleState);
  EXPECT_EQ(Call.PC, StatePC);
  EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Context, 0, 2}));
  acknowledge(Call);
  success(Model.finishCallback(Call.Token));
  EXPECT_EQ(take(Model.component(Handle, 0)).IdleState, 2u);
  success(Model.activate(Handle, 0));
  EXPECT_FALSE(take(Model.conditionReached(Handle, 0, true)));
  Call = next(Kind::IdleState);
  EXPECT_EQ(Call.State, 0u);
  success(Model.finishCallback(Call.Token));
  EXPECT_FALSE(Model.nextCallback());
  success(Model.completeIdleState(Handle, 0));
  Call = next(Kind::ActiveCondition);
  EXPECT_EQ(take(Model.component(Handle, 0)).IdleState, 0u);
  EXPECT_TRUE(take(Model.component(Handle, 0)).Active);
  EXPECT_FALSE(take(Model.conditionReached(Handle, 0, true)));
  success(Model.finishCallback(Call.Token));
  EXPECT_TRUE(take(Model.conditionReached(Handle, 0, true)));
}

TEST_F(DriverKernelPoFx,
       ReactivationDuringIdleDeliveryWaitsForBothAcknowledgments) {
  registerDevice();
  success(Model.start(Handle));
  const auto Call = next(Kind::IdleCondition);
  success(Model.activate(Handle, 0));
  EXPECT_FALSE(Model.nextCallback());
  success(Model.completeIdleCondition(Handle, 0));
  EXPECT_FALSE(Model.nextCallback());
  success(Model.finishCallback(Call.Token));
  complete(Kind::ActiveCondition);
  EXPECT_TRUE(take(Model.conditionReached(Handle, 0, true)));
  EXPECT_EQ(take(Model.component(Handle, 0)).References, 1u);
}

TEST_F(DriverKernelPoFx,
       ActivationDuringFxEntryQueuesF0OnlyAfterEntryCompletion) {
  startIdle();
  success(Model.requestIdleState(Handle, 0, 2));
  const auto Enter = next(Kind::IdleState);
  success(Model.activate(Handle, 0));
  EXPECT_FALSE(Model.nextCallback());
  acknowledge(Enter);
  success(Model.finishCallback(Enter.Token));
  const auto Leave = next(Kind::IdleState);
  EXPECT_EQ(Leave.State, 0u);
  acknowledge(Leave);
  success(Model.finishCallback(Leave.Token));
  complete(Kind::ActiveCondition);
}

TEST_F(DriverKernelPoFx,
       BlockingContinuationRetainsItsThreadAcrossF0AndActivation) {
  startIdle();
  success(Model.requestIdleState(Handle, 0, 1));
  complete(Kind::IdleState);
  constexpr uint64_t Thread = 0x4000;
  success(Model.activate(Handle, 0, Thread));
  auto Call = next(Kind::IdleState);
  EXPECT_EQ(Call.Thread, Thread);
  expectError(Model.activate(Handle, 0, Thread), "overlaps");
  EXPECT_EQ(take(Model.component(Handle, 0)).References, 1u);
  acknowledge(Call);
  success(Model.finishCallback(Call.Token));
  Call = next(Kind::ActiveCondition);
  EXPECT_EQ(Call.Thread, Thread);
  success(Model.finishCallback(Call.Token));
  success(Model.idle(Handle, 0, Thread));
  Call = next(Kind::IdleCondition);
  EXPECT_EQ(Call.Thread, Thread);
  acknowledge(Call);
  success(Model.finishCallback(Call.Token));
  success(Model.activate(Handle, 0));
  Call = next(Kind::ActiveCondition);
  EXPECT_EQ(Call.Thread, 0u);
  success(Model.finishCallback(Call.Token));
}

TEST_F(DriverKernelPoFx, FxDecisionsRespectLatencyResidencyAndWakeHints) {
  startIdle();
  success(Model.setLatency(Handle, 0, 20));
  expectError(Model.requestIdleState(Handle, 0, 2), "timing hint");
  success(Model.requestIdleState(Handle, 0, 1));
  complete(Kind::IdleState);
  success(Model.setLatency(Handle, 0, pofx::UnknownTime));
  success(Model.setResidency(Handle, 0, 100));
  expectError(Model.requestIdleState(Handle, 0, 2), "timing hint");
  success(Model.setResidency(Handle, 0, 1000));
  success(Model.setWake(Handle, 0, true));
  expectError(Model.requestIdleState(Handle, 0, 2), "wake hint");
  success(Model.setWake(Handle, 0, false));
  success(Model.requestIdleState(Handle, 0, 2));
  complete(Kind::IdleState);
  const auto C = take(Model.component(Handle, 0));
  EXPECT_EQ(C.IdleState, 2u);
  EXPECT_EQ(C.Latency, pofx::UnknownTime);
  EXPECT_EQ(C.Residency, 1000u);
  EXPECT_FALSE(C.Wake);
}

TEST_F(DriverKernelPoFx, SupersedingAsyncActivationKeepsItsOwnCallbackThread) {
  registerDevice();
  success(Model.activate(Handle, 0));
  success(Model.start(Handle));
  constexpr uint64_t Thread = 0x4000;
  success(Model.idle(Handle, 0, Thread));
  const auto Idle = next(Kind::IdleCondition);
  EXPECT_EQ(Idle.Thread, Thread);
  success(Model.finishCallback(Idle.Token));
  success(Model.activate(Handle, 0));
  EXPECT_EQ(Model.callback(Idle.Token)->Thread, Thread);
  success(Model.completeIdleCondition(Handle, 0));
  EXPECT_FALSE(Model.callback(Idle.Token));
  const auto Active = next(Kind::ActiveCondition);
  EXPECT_EQ(Active.Thread, 0u);
  success(Model.finishCallback(Active.Token));
}

TEST_F(DriverKernelPoFx,
       ExtraAsyncReferencePreservesTheOriginalBlockingThread) {
  startIdle();
  constexpr uint64_t Thread = 0x4000;
  success(Model.activate(Handle, 0, Thread));
  success(Model.activate(Handle, 0));
  const auto Active = next(Kind::ActiveCondition);
  EXPECT_EQ(Active.Thread, Thread);
  success(Model.finishCallback(Active.Token));
  success(Model.idle(Handle, 0, Thread));
  EXPECT_FALSE(Model.nextCallback());
  EXPECT_EQ(take(Model.component(Handle, 0)).References, 1u);
  EXPECT_TRUE(take(Model.conditionReached(Handle, 0, true)));
  success(Model.idle(Handle, 0));
  const auto Idle = next(Kind::IdleCondition);
  EXPECT_EQ(Idle.Thread, 0u);
  acknowledge(Idle);
  success(Model.finishCallback(Idle.Token));
}

TEST_F(DriverKernelPoFx, UnknownStateTimingIsRetainedButNeverGuessed) {
  auto Description = description();
  Description.Components[0].IdleStates[1].TransitionLatency = pofx::UnknownTime;
  success(Model.registerDevice(Handle, Description));
  success(Model.start(Handle));
  complete(Kind::IdleCondition);
  expectError(Model.requestIdleState(Handle, 0, 1), "unknown timing");
  EXPECT_EQ(take(Model.component(Handle, 0)).IdleState, 0u);
  success(Model.requestIdleState(Handle, 0, 2));
  complete(Kind::IdleState);
}

TEST_F(DriverKernelPoFx, PowerDownWaitsForEveryComponentAndAnExplicitDecision) {
  registerDevice(2);
  success(Model.start(Handle));
  complete(Kind::IdleCondition);
  expectError(Model.requestDevicePowerNotRequired(Handle), "quiescent");
  complete(Kind::IdleCondition, 1);
  EXPECT_FALSE(Model.nextCallback());
  success(Model.process(1000));
  EXPECT_FALSE(Model.nextCallback());
  success(Model.requestDevicePowerNotRequired(Handle));
  complete(Kind::DevicePowerNotRequired);
  success(Model.activate(Handle, 1));
  const auto Power = next(Kind::DevicePowerRequired);
  EXPECT_EQ(Power.Arguments, (std::vector<uint64_t>{Context}));
  success(Model.finishCallback(Power.Token));
  EXPECT_FALSE(Model.nextCallback());
  success(Model.reportDevicePoweredOn(Handle));
  complete(Kind::ActiveCondition, 1);
  EXPECT_TRUE(take(Model.conditionReached(Handle, 1, true)));
}

TEST_F(DriverKernelPoFx, PowerCallbacksRequireTheirOwnDeliveredCompletions) {
  startIdle();
  expectError(Model.reportDevicePoweredOn(Handle), "no pending");
  success(Model.requestDevicePowerNotRequired(Handle));
  const auto NotRequired = *Model.nextCallback();
  expectError(Model.completeDevicePowerNotRequired(Handle), "delivered");
  const auto Call = next(Kind::DevicePowerNotRequired);
  expectError(Model.reportDevicePoweredOn(Handle), "matching");
  success(Model.activate(Handle, 0));
  EXPECT_FALSE(Model.nextCallback());
  acknowledge(Call);
  EXPECT_FALSE(Model.nextCallback());
  success(Model.finishCallback(NotRequired.Token));
  complete(Kind::DevicePowerRequired);
  complete(Kind::ActiveCondition);
}

TEST_F(DriverKernelPoFx,
       DeviceIdleTimeoutUsesVirtualTimeAndCancelsOnActivation) {
  success(Model.process(100));
  startIdle();
  success(Model.setDeviceIdleTimeout(Handle, 50));
  success(Model.requestDevicePowerNotRequired(Handle));
  EXPECT_EQ(Model.nextDeadline(), 150u);
  success(Model.process(149));
  EXPECT_FALSE(Model.nextCallback());
  success(Model.activate(Handle, 0));
  EXPECT_FALSE(Model.nextDeadline());
  complete(Kind::ActiveCondition);
  success(Model.process(151));
  EXPECT_FALSE(Model.nextCallback());
  success(Model.idle(Handle, 0));
  complete(Kind::IdleCondition);
  success(Model.requestDevicePowerNotRequired(Handle));
  EXPECT_EQ(Model.nextDeadline(), 201u);
  success(Model.process(201));
  EXPECT_FALSE(Model.nextDeadline());
  complete(Kind::DevicePowerNotRequired);
}

TEST_F(DriverKernelPoFx, IdleTimeoutChangesAndOverflowAreFailureAtomic) {
  success(Model.process(10));
  startIdle();
  success(Model.setDeviceIdleTimeout(Handle, 50));
  success(Model.requestDevicePowerNotRequired(Handle));
  EXPECT_EQ(Model.nextDeadline(), 60u);
  success(Model.setDeviceIdleTimeout(Handle, 70));
  EXPECT_EQ(Model.nextDeadline(), 80u);
  expectError(Model.setDeviceIdleTimeout(Handle, UINT64_MAX), "overflows");
  EXPECT_EQ(Model.nextDeadline(), 80u);
  expectError(Model.process(9), "backwards");
  EXPECT_EQ(Model.nextDeadline(), 80u);
  success(Model.process(80));
  complete(Kind::DevicePowerNotRequired);
}

TEST_F(DriverKernelPoFx,
       AbsentF0CallbacksChangeConditionWithoutInventingCalls) {
  auto D = description();
  D.Routines = {};
  D.Components[0].IdleStates.resize(1);
  D.Components[0].DeepestWakeableState = 0;
  success(Model.registerDevice(Handle, D));
  success(Model.start(Handle));
  EXPECT_TRUE(take(Model.conditionReached(Handle, 0, false)));
  success(Model.activate(Handle, 0));
  EXPECT_TRUE(take(Model.conditionReached(Handle, 0, true)));
  success(Model.idle(Handle, 0));
  EXPECT_TRUE(take(Model.conditionReached(Handle, 0, false)));
  EXPECT_FALSE(Model.hasPendingCallbacks());
  expectError(Model.requestDevicePowerNotRequired(Handle), "no power");
}

TEST_F(DriverKernelPoFx, FrameworkOwnedRegistrationUsesTypedInternalCallbacks) {
  auto D = description();
  D.Routines = {};
  D.Owner = KernelPoFx::RegistrationOwner::Framework;
  D.Components[0].IdleStates.resize(1);
  D.Components[0].DeepestWakeableState = 0;
  success(Model.registerDevice(Handle, D));
  success(Model.start(Handle));
  auto Call = next(Kind::IdleCondition);
  EXPECT_TRUE(Call.Internal);
  EXPECT_EQ(Call.PC, 0u);
  acknowledge(Call);
  success(Model.finishCallback(Call.Token));
  success(Model.requestDevicePowerNotRequired(Handle));
  Call = next(Kind::DevicePowerNotRequired);
  EXPECT_TRUE(Call.Internal);
  EXPECT_EQ(Call.PC, 0u);
  acknowledge(Call);
  success(Model.finishCallback(Call.Token));
  success(Model.activate(Handle, 0));
  complete(Kind::DevicePowerRequired);
  complete(Kind::ActiveCondition);
  EXPECT_TRUE(take(Model.conditionReached(Handle, 0, true)));
}

TEST_F(DriverKernelPoFx,
       FrameworkOwnershipValidatesComponentAndCallbackBounds) {
  auto D = description();
  D.Owner = static_cast<KernelPoFx::RegistrationOwner>(99);
  expectError(Model.registerDevice(Handle, D), "invalid owner");
  D.Owner = KernelPoFx::RegistrationOwner::Framework;
  expectError(Model.registerDevice(Handle, D), "owns device-power callbacks");
  D.Routines.DevicePowerRequired = D.Routines.DevicePowerNotRequired = 0;
  D.Components.push_back(D.Components.front());
  expectError(Model.registerDevice(Handle, D), "exactly one component");
  EXPECT_FALSE(Model.registration(Handle));
  D.Components.pop_back();
  success(Model.registerDevice(Handle, D));
  EXPECT_EQ(Model.registration(Handle)->Owner,
            KernelPoFx::RegistrationOwner::Framework);
}

TEST_F(DriverKernelPoFx,
       FrameworkGuestComponentsKeepInternalDevicePowerOwnership) {
  auto D = description();
  D.Owner = KernelPoFx::RegistrationOwner::Framework;
  D.Routines.DevicePowerRequired = D.Routines.DevicePowerNotRequired = 0;
  success(Model.registerDevice(Handle, D));
  success(Model.start(Handle));
  auto Call = next(Kind::IdleCondition);
  EXPECT_FALSE(Call.Internal);
  EXPECT_EQ(Call.PC, IdlePC);
  acknowledge(Call);
  success(Model.finishCallback(Call.Token));
  success(Model.requestIdleState(Handle, 0, 1));
  Call = next(Kind::IdleState);
  EXPECT_FALSE(Call.Internal);
  EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Context, 0, 1}));
  acknowledge(Call);
  success(Model.finishCallback(Call.Token));
  success(Model.requestDevicePowerNotRequired(Handle));
  Call = next(Kind::DevicePowerNotRequired);
  EXPECT_TRUE(Call.Internal);
  EXPECT_EQ(Call.PC, 0u);
  EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Context}));
  acknowledge(Call);
  success(Model.finishCallback(Call.Token));
  success(Model.activate(Handle, 0));
  Call = next(Kind::DevicePowerRequired);
  EXPECT_TRUE(Call.Internal);
  EXPECT_EQ(Call.PC, 0u);
  acknowledge(Call);
  success(Model.finishCallback(Call.Token));
  Call = next(Kind::IdleState);
  EXPECT_FALSE(Call.Internal);
  EXPECT_EQ(Call.State, 0u);
  acknowledge(Call);
  success(Model.finishCallback(Call.Token));
  Call = next(Kind::ActiveCondition);
  EXPECT_FALSE(Call.Internal);
  EXPECT_EQ(Call.PC, ActivePC);
  success(Model.finishCallback(Call.Token));
  EXPECT_TRUE(take(Model.callbacksDrained(Handle)));
}

TEST_F(DriverKernelPoFx,
       FrameworkDefaultsRemainAvailableForEachOmittedComponentCallback) {
  for (bool GuestStateCallback : {false, true}) {
    SCOPED_TRACE(GuestStateCallback);
    Model = KernelPoFx{};
    auto D = description();
    D.Owner = KernelPoFx::RegistrationOwner::Framework;
    D.Routines = {};
    if (GuestStateCallback)
      D.Routines.IdleState = StatePC;
    success(Model.registerDevice(Handle, D));
    success(Model.start(Handle));
    auto Call = next(Kind::IdleCondition);
    EXPECT_TRUE(Call.Internal);
    acknowledge(Call);
    success(Model.finishCallback(Call.Token));
    success(Model.requestIdleState(Handle, 0, 1));
    Call = next(Kind::IdleState);
    EXPECT_EQ(Call.Internal, !GuestStateCallback);
    EXPECT_EQ(Call.PC, GuestStateCallback ? StatePC : 0u);
    acknowledge(Call);
    success(Model.finishCallback(Call.Token));
    success(Model.activate(Handle, 0));
    Call = next(Kind::IdleState);
    EXPECT_EQ(Call.Internal, !GuestStateCallback);
    EXPECT_EQ(Call.State, 0u);
    acknowledge(Call);
    success(Model.finishCallback(Call.Token));
    Call = next(Kind::ActiveCondition);
    EXPECT_TRUE(Call.Internal);
    EXPECT_EQ(Call.PC, 0u);
    success(Model.finishCallback(Call.Token));
    EXPECT_TRUE(take(Model.conditionReached(Handle, 0, true)));
  }
}

TEST_F(DriverKernelPoFx,
       CallbackDrainTracksLiveRegistrationThroughReturnAndAcknowledgement) {
  auto Unknown = Model.callbacksDrained(Handle);
  ASSERT_FALSE(bool(Unknown));
  EXPECT_NE(llvm::toString(Unknown.takeError()).find("live registration"),
            std::string::npos);
  registerDevice();
  EXPECT_TRUE(take(Model.callbacksDrained(Handle)));
  success(Model.start(Handle));
  EXPECT_FALSE(take(Model.callbacksDrained(Handle)));
  const auto Call = next(Kind::IdleCondition);
  EXPECT_FALSE(take(Model.callbacksDrained(Handle)));
  success(Model.finishCallback(Call.Token));
  EXPECT_FALSE(take(Model.callbacksDrained(Handle)));
  success(Model.completeIdleCondition(Handle, 0));
  EXPECT_TRUE(take(Model.callbacksDrained(Handle)));
  success(Model.setDeviceIdleTimeout(Handle, 100));
  success(Model.requestDevicePowerNotRequired(Handle));
  EXPECT_TRUE(take(Model.callbacksDrained(Handle)));
  success(Model.unregisterDevice(Handle));
  Unknown = Model.callbacksDrained(Handle);
  ASSERT_FALSE(bool(Unknown));
  llvm::consumeError(Unknown.takeError());
}

TEST_F(DriverKernelPoFx, FrameworkQuiescenceCancelsUnissuedPowerDecisions) {
  auto D = description();
  D.Owner = KernelPoFx::RegistrationOwner::Framework;
  D.Routines.DevicePowerRequired = D.Routines.DevicePowerNotRequired = 0;
  success(Model.registerDevice(Handle, D));
  success(Model.start(Handle));
  complete(Kind::IdleCondition);
  success(Model.setDeviceIdleTimeout(Handle, 100));
  success(Model.requestDevicePowerNotRequired(Handle));
  EXPECT_EQ(Model.nextDeadline(), 100u);
  success(Model.quiesceFrameworkRegistration(Handle));
  success(Model.quiesceFrameworkRegistration(Handle));
  EXPECT_FALSE(Model.nextDeadline());
  success(Model.process(1000));
  EXPECT_FALSE(Model.hasPendingCallbacks());
  expectError(Model.requestIdleState(Handle, 0, 1), "quiescing");
  expectError(Model.requestDevicePowerNotRequired(Handle), "quiescing");
  expectError(Model.activate(Handle, 0), "quiescing");
  expectError(Model.idle(Handle, 0), "quiescing");
  success(Model.setLatency(Handle, 0, 20));
  success(Model.setResidency(Handle, 0, 30));
  success(Model.setWake(Handle, 0, true));
  EXPECT_TRUE(Model.registration(Handle));
  success(Model.unregisterDevice(Handle));
}

TEST_F(DriverKernelPoFx, FrameworkQuiescenceRetainsExistingCallbacks) {
  for (bool BeforeEntry : {false, true}) {
    SCOPED_TRACE(BeforeEntry);
    Model = KernelPoFx{};
    auto D = description();
    D.Owner = KernelPoFx::RegistrationOwner::Framework;
    D.Routines.DevicePowerRequired = D.Routines.DevicePowerNotRequired = 0;
    success(Model.registerDevice(Handle, D));
    success(Model.start(Handle));
    complete(Kind::IdleCondition);
    success(Model.requestIdleState(Handle, 0, 1));
    if (BeforeEntry)
      success(Model.quiesceFrameworkRegistration(Handle));
    const auto State = next(Kind::IdleState);
    if (!BeforeEntry)
      success(Model.quiesceFrameworkRegistration(Handle));
    success(Model.process(100));
    EXPECT_FALSE(take(Model.callbacksDrained(Handle)));
    expectError(Model.unregisterDevice(Handle), "pending callbacks");
    success(Model.finishCallback(State.Token));
    EXPECT_FALSE(take(Model.callbacksDrained(Handle)));
    acknowledge(State);
    EXPECT_TRUE(take(Model.callbacksDrained(Handle)));
    EXPECT_EQ(take(Model.component(Handle, 0)).IdleState, 1u);
    EXPECT_FALSE(Model.hasPendingCallbacks());
    success(Model.unregisterDevice(Handle));
  }
}

TEST_F(DriverKernelPoFx, FrameworkQuiescencePreservesPendingPowerCompletion) {
  auto D = description();
  D.Owner = KernelPoFx::RegistrationOwner::Framework;
  D.Routines.DevicePowerRequired = D.Routines.DevicePowerNotRequired = 0;
  success(Model.registerDevice(Handle, D));
  success(Model.start(Handle));
  complete(Kind::IdleCondition);
  success(Model.requestDevicePowerNotRequired(Handle));
  const auto Pending = Model.nextCallback();
  ASSERT_TRUE(Pending);
  expectError(Model.quiesceFrameworkRegistration(Handle), "device-power");
  ASSERT_TRUE(Model.nextCallback());
  EXPECT_EQ(Model.nextCallback()->Token, Pending->Token);
  const auto Power = next(Kind::DevicePowerNotRequired);
  expectError(Model.quiesceFrameworkRegistration(Handle), "device-power");
  success(Model.finishCallback(Power.Token));
  expectError(Model.quiesceFrameworkRegistration(Handle), "device-power");
  success(Model.completeDevicePowerNotRequired(Handle));
  success(Model.quiesceFrameworkRegistration(Handle));
  success(Model.unregisterDevice(Handle));
}

TEST_F(DriverKernelPoFx,
       FailedFrameworkPowerUpQuiescesExactCallbackWithoutActivatingComponents) {
  auto D = description();
  D.Owner = KernelPoFx::RegistrationOwner::Framework;
  D.Routines.DevicePowerRequired = D.Routines.DevicePowerNotRequired = 0;
  success(Model.registerDevice(Handle, D));
  success(Model.start(Handle));
  complete(Kind::IdleCondition);
  success(Model.requestIdleState(Handle, 0, 1));
  complete(Kind::IdleState);
  success(Model.requestDevicePowerNotRequired(Handle));
  complete(Kind::DevicePowerNotRequired);
  success(Model.activate(Handle, 0));
  auto Pending = Model.nextCallback();
  ASSERT_TRUE(Pending);
  expectError(Model.quiesceFrameworkRegistration(Handle, Pending->Token),
              "exact returned callback");
  const auto Required = next(Kind::DevicePowerRequired);
  expectError(Model.quiesceFrameworkRegistration(Handle, Required.Token),
              "exact returned callback");
  success(Model.finishCallback(Required.Token));
  expectError(Model.quiesceFrameworkRegistration(Handle, Required.Token + 1),
              "exact returned callback");
  ASSERT_NE(Model.callback(Required.Token), nullptr);
  EXPECT_FALSE(take(Model.callbacksDrained(Handle)));
  success(Model.quiesceFrameworkRegistration(Handle, Required.Token));
  EXPECT_EQ(Model.callback(Required.Token), nullptr);
  EXPECT_FALSE(Model.nextCallback());
  EXPECT_TRUE(take(Model.callbacksDrained(Handle)));
  const auto Component = take(Model.component(Handle, 0));
  EXPECT_FALSE(Component.Active);
  EXPECT_EQ(Component.IdleState, 1u);
  expectError(Model.quiesceFrameworkRegistration(Handle, Required.Token),
              "exact returned callback");
  success(Model.unregisterDevice(Handle));
}

TEST_F(DriverKernelPoFx, FrameworkQuiescenceDoesNotAlterDriverOwnership) {
  registerDevice();
  expectError(Model.quiesceFrameworkRegistration(Handle), "framework-owned");
  success(Model.start(Handle));
  complete(Kind::IdleCondition);
  success(Model.activate(Handle, 0));
  complete(Kind::ActiveCondition);
}

TEST_F(DriverKernelPoFx,
       CallbackCapacityFailureDoesNotPartiallyStartComponents) {
  KernelPoFx::Limits Bounds;
  Bounds.MaxCallbacks = 1;
  Model = KernelPoFx(Bounds);
  registerDevice(2);
  expectError(Model.start(Handle), "callback bound");
  EXPECT_FALSE(Model.hasPendingCallbacks());
  EXPECT_TRUE(take(Model.component(Handle, 0)).Active);
  EXPECT_TRUE(take(Model.component(Handle, 1)).Active);
  success(Model.activate(Handle, 0));
  success(Model.start(Handle));
  EXPECT_EQ(Model.nextCallback()->Token, 1u);
  complete(Kind::IdleCondition, 1);
}

TEST_F(DriverKernelPoFx, CallbackBoundRollsBackReferencesAndCanRetry) {
  KernelPoFx::Limits Bounds;
  Bounds.MaxCallbacks = 1;
  Model = KernelPoFx(Bounds);
  registerDevice(2);
  success(Model.activate(Handle, 0));
  success(Model.start(Handle));
  expectError(Model.idle(Handle, 0), "callback bound");
  EXPECT_EQ(take(Model.component(Handle, 0)).References, 1u);
  complete(Kind::IdleCondition, 1);
  success(Model.idle(Handle, 0));
  complete(Kind::IdleCondition);
}

TEST_F(DriverKernelPoFx, DeadlineBatchesAreAtomicAcrossRegistrations) {
  KernelPoFx::Limits Bounds;
  Bounds.MaxCallbacks = 1;
  Model = KernelPoFx(Bounds);
  registerDevice();
  success(Model.start(Handle));
  complete(Kind::IdleCondition);
  auto D = description();
  D.PDO += 1;
  success(Model.registerDevice(Handle + 1, D));
  success(Model.start(Handle + 1));
  auto Second = *Model.nextCallback();
  success(Model.submitCallback(Second.Token));
  success(Model.beginCallback(Second.Token));
  acknowledge(Second);
  success(Model.finishCallback(Second.Token));
  success(Model.setDeviceIdleTimeout(Handle, 100));
  success(Model.setDeviceIdleTimeout(Handle + 1, 100));
  success(Model.requestDevicePowerNotRequired(Handle));
  success(Model.requestDevicePowerNotRequired(Handle + 1));
  expectError(Model.process(100), "callback bound");
  EXPECT_FALSE(Model.hasPendingCallbacks());
  EXPECT_EQ(Model.nextDeadline(), 100u);
  success(Model.setDeviceIdleTimeout(Handle + 1, 200));
  success(Model.process(100));
  complete(Kind::DevicePowerNotRequired);
  EXPECT_EQ(Model.nextDeadline(), 200u);
}

TEST_F(DriverKernelPoFx, UnregisterCancelsUndeliveredIdleDeadline) {
  startIdle();
  success(Model.setDeviceIdleTimeout(Handle, 100));
  success(Model.requestDevicePowerNotRequired(Handle));
  EXPECT_TRUE(Model.nextDeadline());
  success(Model.unregisterDevice(Handle));
  EXPECT_FALSE(Model.nextDeadline());
  success(Model.process(100));
  EXPECT_FALSE(Model.hasPendingCallbacks());
  success(Model.canReleasePDO(PDO));
}
TEST_F(DriverKernelPoFx, AsyncIdleCannotCancelARetainedBlockingActivation) {
  for (bool DevicePower : {false, true}) {
    SCOPED_TRACE(DevicePower);
    Model = KernelPoFx{};
    startIdle();
    if (DevicePower) {
      success(Model.requestDevicePowerNotRequired(Handle));
      complete(Kind::DevicePowerNotRequired);
    } else {
      success(Model.requestIdleState(Handle, 0, 1));
      complete(Kind::IdleState);
    }
    constexpr uint64_t Thread = 0x4000;
    success(Model.activate(Handle, 0, Thread));
    const auto Intermediate =
        next(DevicePower ? Kind::DevicePowerRequired : Kind::IdleState);
    EXPECT_EQ(Intermediate.Thread, Thread);
    success(Model.finishCallback(Intermediate.Token));
    success(Model.idle(Handle, 0));
    EXPECT_EQ(take(Model.component(Handle, 0)).References, 0u);
    expectError(Model.activate(Handle, 0, Thread + 1), "overlaps");
    acknowledge(Intermediate);
    const auto Active = next(Kind::ActiveCondition);
    EXPECT_EQ(Active.Thread, Thread);
    EXPECT_EQ(take(Model.component(Handle, 0)).ActiveGeneration, 0u);
    success(Model.finishCallback(Active.Token));
    EXPECT_EQ(take(Model.component(Handle, 0)).ActiveGeneration, 1u);
    const auto Idle = next(Kind::IdleCondition);
    EXPECT_EQ(Idle.Thread, 0u);
    acknowledge(Idle);
    success(Model.finishCallback(Idle.Token));
    EXPECT_TRUE(take(Model.conditionReached(Handle, 0, false)));
    EXPECT_EQ(take(Model.component(Handle, 0)).IdleGeneration, 2u);
  }
}

TEST_F(DriverKernelPoFx,
       OptionalCallbacksStillRetireTheBlockingActivationObligation) {
  auto D = description();
  D.Routines.ActiveCondition = D.Routines.IdleCondition = D.Routines.IdleState =
      0;
  D.Components[0].IdleStates.resize(1);
  D.Components[0].DeepestWakeableState = 0;
  success(Model.registerDevice(Handle, std::move(D)));
  success(Model.start(Handle));
  success(Model.requestDevicePowerNotRequired(Handle));
  complete(Kind::DevicePowerNotRequired);
  success(Model.activate(Handle, 0, 0x4000));
  const auto Required = next(Kind::DevicePowerRequired);
  success(Model.finishCallback(Required.Token));
  success(Model.idle(Handle, 0));
  success(Model.reportDevicePoweredOn(Handle));
  EXPECT_FALSE(Model.hasPendingCallbacks());
  EXPECT_TRUE(take(Model.conditionReached(Handle, 0, false)));
  const auto C = take(Model.component(Handle, 0));
  EXPECT_EQ(C.ActiveGeneration, 1u);
  EXPECT_EQ(C.IdleGeneration, 2u);
  success(Model.requestDevicePowerNotRequired(Handle));
  complete(Kind::DevicePowerNotRequired);
  success(Model.unregisterDevice(Handle));
}

TEST_F(DriverKernelPoFx,
       PowerAcknowledgementPreflightRetainsExactDeliveredCallback) {
  startIdle();
  success(Model.requestDevicePowerNotRequired(Handle));
  const auto Call = Model.nextCallback();
  ASSERT_TRUE(Call);
  ASSERT_EQ(Call->Kind, Kind::DevicePowerNotRequired);
  expectError(Model.canCompleteDevicePowerNotRequired(Handle, Call->Token),
              "delivered callback");
  success(Model.submitCallback(Call->Token));
  expectError(Model.canCompleteDevicePowerNotRequired(Handle, Call->Token),
              "delivered callback");
  success(Model.beginCallback(Call->Token));
  success(Model.finishCallback(Call->Token));
  success(Model.activate(Handle, 0));
  EXPECT_FALSE(Model.nextCallback());
  expectError(Model.canCompleteDevicePowerNotRequired(Handle + 1, Call->Token),
              "exact callback");
  expectError(Model.canCompleteDevicePowerNotRequired(Handle, Call->Token + 1),
              "exact callback");
  success(Model.canCompleteDevicePowerNotRequired(Handle, Call->Token));
  success(Model.canCompleteDevicePowerNotRequired(Handle, Call->Token));
  EXPECT_TRUE(Model.callback(Call->Token));
  EXPECT_FALSE(Model.nextCallback());
  success(Model.completeDevicePowerNotRequired(Handle));
  expectError(Model.canCompleteDevicePowerNotRequired(Handle, Call->Token),
              "exact callback");
  const auto Required = next(Kind::DevicePowerRequired);
  expectError(Model.canCompleteDevicePowerNotRequired(Handle, Required.Token),
              "exact callback");
  acknowledge(Required);
  success(Model.finishCallback(Required.Token));
  complete(Kind::ActiveCondition);
  EXPECT_TRUE(take(Model.conditionReached(Handle, 0, true)));
}

} // namespace
} // namespace neverd::emulation
