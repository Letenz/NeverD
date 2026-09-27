//===- KernelD3ColdResourceTests.cpp - Cold bus power authority ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check power generations, resource epochs and MMIO access across cold cycles.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/KernelMMIO.h"

namespace neverd::emulation {
namespace {

class KernelD3ColdResourceTest : public ::testing::Test {
protected:
  static constexpr uint64_t PDO = 0x2000, Physical = 0x80000000;
  bool Busy = false;
  std::unique_ptr<UnicornBackend> Memory;
  KernelResources Resources{[](uint64_t) { return llvm::Error::success(); },
                            {},
                            [this](uint64_t) -> llvm::Error {
                              if (Busy)
                                return llvm::createStringError(
                                    llvm::inconvertibleErrorCode(),
                                    "active hardware transaction");
                              return llvm::Error::success();
                            }};
  std::unique_ptr<KernelMMIO> MMIO;

  void ok(llvm::Error Error) {
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
  DriverPnpDevice configuration(bool Registers = true) {
    DriverPnpDevice Device;
    Device.ID = "cold-bus";
    Device.Bus =
        Registers ? DriverBusKind::RegisterBank : DriverBusKind::ResourceFree;
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    Device.WakeCapabilities = DriverWakeCapabilities{true, true};
    Device.D3Cold = DriverD3ColdCapabilities{true, false, true, false};
    if (Registers)
      Device.Resources.push_back(
          {"bar0",
           Physical,
           Physical,
           16,
           {{0, 4, DriverRegisterAccess::ReadWrite, 17},
            {4, 4, DriverRegisterAccess::ReadWrite, 23}}});
    return Device;
  }
  void configure(DriverPnpDevice Device) {
    ok(Resources.configure(PDO, Device));
    ok(MMIO->configure(PDO));
  }
  void start() {
    ok(Resources.beginStart(PDO));
    ok(Resources.completeLowerStart(PDO, 0));
    ok(Resources.finishPnp(PDO, DevicePnpRequest::Start, 0));
  }
  void cold(bool RequireWake = true) {
    Resources.setPhysicalPower(PDO, DevicePowerState::D3);
    ok(Resources.enterD3Cold(PDO, SystemPowerState::Working, RequireWake));
  }
  void SetUp() override {
    Memory = take(UnicornBackend::create(16 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    MMIO = std::make_unique<KernelMMIO>(*Memory, Resources);
  }
};

TEST_F(KernelD3ColdResourceTest,
       WakeCapableInterruptHasTheDocumentedResourceHint) {
  auto Device = configuration(false);
  Device.Bus = DriverBusKind::RegisterBank;
  DriverInterruptResource Interrupt;
  Interrupt.ID = "wake-irq";
  Interrupt.RawVector = 19;
  Interrupt.RawLevel = 5;
  Interrupt.RawAffinity = 1;
  Interrupt.TranslatedVector = 81;
  Interrupt.TranslatedLevel = 5;
  Interrupt.TranslatedAffinity = 1;
  Interrupt.WakeCapable = true;
  Device.Interrupts.push_back(Interrupt);
  configure(Device);
  for (bool Translated : {false, true}) {
    const auto List = take(Resources.resourceList(PDO, Translated));
    ASSERT_GT(List.size(),
              resources::ResourceHeaderSize + resources::ResourceFlagsOffset);
    EXPECT_EQ(
        List[resources::ResourceHeaderSize + resources::ResourceFlagsOffset],
        resources::InterruptLatched | resources::InterruptWakeHint);
  }
}

TEST_F(KernelD3ColdResourceTest,
       ColdTransitionsPreserveAssignmentAndSupplyFacts) {
  configure(configuration());
  EXPECT_TRUE(Resources.supportsD3Cold(PDO));
  EXPECT_FALSE(Resources.d3ColdEnabledByDefault(PDO));
  reject(Resources.enterD3Cold(PDO, SystemPowerState::Working, true), "START");
  start();
  const auto Epoch = Resources.find(PDO)->Epoch;
  reject(Resources.enterD3Cold(PDO, SystemPowerState::Working, true), "D3hot");
  cold();
  EXPECT_TRUE(Resources.isD3Cold(PDO));
  EXPECT_EQ(Resources.find(PDO)->Power, DevicePowerState::D3);
  EXPECT_EQ(Resources.find(PDO)->PowerGeneration, 1u);
  EXPECT_EQ(Resources.find(PDO)->Epoch, Epoch);
  EXPECT_TRUE(Resources.find(PDO)->Assigned);
  EXPECT_TRUE(Resources.find(PDO)->Present);
  reject(Resources.enterD3Cold(PDO, SystemPowerState::Working, true), "D3hot");
  Resources.setPhysicalPower(PDO, DevicePowerState::D3);
  EXPECT_TRUE(Resources.isD3Cold(PDO));
  Resources.setPhysicalPower(PDO, DevicePowerState::D0);
  EXPECT_FALSE(Resources.isD3Cold(PDO));
  EXPECT_EQ(Resources.find(PDO)->PowerGeneration, 1u);
  cold();
  EXPECT_EQ(Resources.find(PDO)->PowerGeneration, 2u);
  EXPECT_EQ(Resources.find(PDO)->Epoch, Epoch);
}

TEST_F(KernelD3ColdResourceTest, CapabilityAndColdWakeAreIndependentOfPolicy) {
  auto Device = configuration();
  Device.D3Cold->EnabledByDefault = true;
  configure(Device);
  start();
  EXPECT_TRUE(Resources.d3ColdEnabledByDefault(PDO));
  Resources.setPhysicalPower(PDO, DevicePowerState::D3);
  reject(Resources.enterD3Cold(PDO, SystemPowerState::Sleeping3, true),
         "wake capability");
  EXPECT_EQ(Resources.find(PDO)->PowerGeneration, 0u);
  ok(Resources.enterD3Cold(PDO, SystemPowerState::Sleeping3, false));
}

TEST_F(KernelD3ColdResourceTest, MissingProviderDoesNotInventColdSupport) {
  auto Device = configuration();
  Device.D3Cold.reset();
  configure(Device);
  start();
  Resources.setPhysicalPower(PDO, DevicePowerState::D3);
  EXPECT_FALSE(Resources.supportsD3Cold(PDO));
  reject(Resources.enterD3Cold(PDO, SystemPowerState::Working, false),
         "explicit bus and platform support");
  EXPECT_FALSE(Resources.isD3Cold(PDO));
  EXPECT_EQ(Resources.find(PDO)->PowerGeneration, 0u);
}

TEST_F(KernelD3ColdResourceTest,
       ResourceFreeProviderOwnsPowerWithoutCMResources) {
  configure(configuration(false));
  EXPECT_FALSE(Resources.hasResources(PDO));
  auto List = Resources.resourceList(PDO, true);
  ASSERT_FALSE(bool(List));
  llvm::consumeError(List.takeError());
  start();
  cold();
  EXPECT_TRUE(Resources.isD3Cold(PDO));
  EXPECT_EQ(Resources.find(PDO)->Epoch, 1u);
}

TEST_F(KernelD3ColdResourceTest,
       FailedColdPreflightPreservesRegistersAndEpoch) {
  configure(configuration());
  start();
  const uint64_t Alias = take(MMIO->map(Physical, 16, mmio::NonCached, false));
  ok(Memory->writeInteger(Alias, 91, 4));
  Resources.setPhysicalPower(PDO, DevicePowerState::D3);
  Busy = true;
  reject(Resources.enterD3Cold(PDO, SystemPowerState::Working, true),
         "active hardware transaction");
  EXPECT_FALSE(Resources.isD3Cold(PDO));
  EXPECT_EQ(Resources.find(PDO)->PowerGeneration, 0u);
  Resources.setPhysicalPower(PDO, DevicePowerState::D0);
  EXPECT_EQ(take(Memory->readInteger(Alias, 4)), 91u);
  EXPECT_EQ(Resources.find(PDO)->Epoch, 1u);
}

TEST_F(KernelD3ColdResourceTest,
       ColdCycleRestoresConfiguredRegistersAcrossAliases) {
  configure(configuration());
  start();
  const uint64_t First = take(MMIO->map(Physical, 16, mmio::NonCached, false));
  const uint64_t Second = take(MMIO->map(Physical, 16, mmio::NonCached, false));
  ok(Memory->writeInteger(First, 91, 4));
  ok(Memory->writeInteger(Second + 4, 92, 4));
  cold();
  EXPECT_FALSE(take(Memory->canAccess(First, 4, GuestPermission::Read)));
  Resources.setPhysicalPower(PDO, DevicePowerState::D0);
  // A write as the first operation must restore all the other registers once.
  ok(Memory->writeInteger(First, 93, 4));
  EXPECT_EQ(take(Memory->readInteger(Second, 4)), 93u);
  EXPECT_EQ(take(Memory->readInteger(First + 4, 4)), 23u);
  EXPECT_EQ(Resources.find(PDO)->Epoch, 1u);
  cold();
  Resources.setPhysicalPower(PDO, DevicePowerState::D0);
  EXPECT_EQ(take(Memory->readInteger(Second, 4)), 17u);
}

TEST_F(KernelD3ColdResourceTest, ColdRegisterAccessFailsBeforeD0) {
  configure(configuration());
  start();
  const uint64_t Alias = take(MMIO->map(Physical, 16, mmio::NonCached, false));
  cold();
  auto Unpowered = Memory->readInteger(Alias, 4);
  ASSERT_FALSE(bool(Unpowered));
  reject(Unpowered.takeError(), "physical device power D0");
  EXPECT_TRUE(Memory->hasDeviceError());
}

TEST_F(KernelD3ColdResourceTest, AbsentOrStoppedHardwareCannotLosePowerAgain) {
  configure(configuration());
  start();
  Resources.setPhysicalPower(PDO, DevicePowerState::D3);
  ok(Resources.finishPnp(PDO, DevicePnpRequest::Stop, 0));
  reject(Resources.enterD3Cold(PDO, SystemPowerState::Working, true), "START");
  start();
  Resources.surpriseRemoval(PDO);
  reject(Resources.enterD3Cold(PDO, SystemPowerState::Working, true), "START");
  EXPECT_EQ(Resources.find(PDO)->PowerGeneration, 0u);
}

} // namespace
} // namespace neverd::emulation
