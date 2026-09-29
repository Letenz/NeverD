//===- KernelPnpTopologyTests.cpp - Provider parent identities ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Keep explicit devnode parents separate from device-stack attachment edges.
///
//===----------------------------------------------------------------------===//

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/DriverImage.h"
#include "os/windows/KernelModel.h"
#include "os/windows/WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace windows;

class KernelPnpParentLifecycle : public ::testing::Test {
protected:
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;

  void success(llvm::Error Error) {
    if (Error)
      ADD_FAILURE() << llvm::toString(std::move(Error));
  }
  template <class T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  template <class T>
  void reject(llvm::Expected<T> Value, llvm::StringRef Message) {
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find(Message.str()),
              std::string::npos);
  }
  void SetUp() override {
    Memory = take(UnicornBackend::create(4 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    DriverOptions Options;
    for (const auto *ID : {"parent", "child"}) {
      DriverPnpDevice Device;
      Device.ID = ID;
      Device.InitialDevicePower = DevicePowerState::D0;
      Device.InitialSystemPower = SystemPowerState::Working;
      Options.PnpDevices.push_back(Device);
    }
    Options.PnpDevices[1].ParentID = "parent";
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + 0x1000;
    Image.Size = 0x3000;
    success(Model->initialize(Image, Options));
    const auto Extension = take(
        Memory->readInteger(Model->driverObject() + DriverExtensionOffset, 8));
    success(Memory->writeInteger(Extension + DriverAddDeviceOffset, Image.Entry,
                                 8));
    success(Model->finishEntry());
    success(Model->preparePnpDevices());
    for (const auto *ID : {"parent", "child"}) {
      take(Model->beginAddDevice(ID));
      success(Model->finishAddDevice(ID, StatusSuccess));
    }
  }
  llvm::Expected<KernelModel::Invocation> begin(llvm::StringRef ID,
                                                DevicePnpRequest Minor) {
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Pnp;
    Request.DeviceID = ID.str();
    Request.Pnp = DriverPnpOperation{Minor, {StatusSuccess, 0}};
    return Model->beginRequest(Request);
  }
  void pnp(llvm::StringRef ID, DevicePnpRequest Minor) {
    SCOPED_TRACE(ID.str());
    SCOPED_TRACE(uint8_t(Minor));
    const auto Call = take(begin(ID, Minor));
    ASSERT_NE(Call.IRP, 0u);
    EXPECT_EQ(Call.PC, 0u);
    success(Model->finalizeRequest(Call.IRP));
  }
  void power(llvm::StringRef ID, DevicePowerState State) {
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Power;
    Request.DeviceID = ID.str();
    DriverPowerOperation Operation;
    Operation.Minor = DevicePowerRequest::Set;
    Operation.Type = DriverPowerType::Device;
    Operation.State = uint32_t(State);
    Operation.BusCompletion = {StatusSuccess, 0};
    Request.Power = Operation;
    const auto Call = take(Model->beginRequest(Request));
    success(Model->finalizeRequest(Call.IRP));
  }
};

TEST_F(KernelPnpParentLifecycle, ChildStartRequiresStartedPoweredParent) {
  reject(begin("child", DevicePnpRequest::Start), "completed START");
  pnp("parent", DevicePnpRequest::Start);
  power("parent", DevicePowerState::D3);
  reject(begin("child", DevicePnpRequest::Start), "stable D0");
  power("parent", DevicePowerState::D0);
  pnp("child", DevicePnpRequest::Start);
  EXPECT_EQ(Result.PnpDevices[1].PnpState, DevicePnpState::Started);
}

TEST_F(KernelPnpParentLifecycle, StopAndRestartFollowProviderDependencyOrder) {
  pnp("parent", DevicePnpRequest::Start);
  pnp("child", DevicePnpRequest::Start);
  pnp("parent", DevicePnpRequest::QueryStop);
  reject(begin("parent", DevicePnpRequest::Stop), "children to stop first");
  EXPECT_EQ(Result.PnpDevices[0].PnpState, DevicePnpState::StopPending);
  pnp("child", DevicePnpRequest::QueryStop);
  pnp("child", DevicePnpRequest::Stop);
  pnp("parent", DevicePnpRequest::Stop);
  reject(begin("child", DevicePnpRequest::Start), "completed START");
  pnp("parent", DevicePnpRequest::Start);
  pnp("child", DevicePnpRequest::Start);
  EXPECT_EQ(Result.PnpDevices[1].PnpState, DevicePnpState::Started);
}

TEST_F(KernelPnpParentLifecycle, RemovalRetainsParentUntilChildrenRetire) {
  reject(begin("parent", DevicePnpRequest::Remove), "child providers");
  pnp("child", DevicePnpRequest::Remove);
  EXPECT_FALSE(Result.PnpDevices[1].ProviderPresent);
  pnp("parent", DevicePnpRequest::Remove);
  EXPECT_FALSE(Result.PnpDevices[0].ProviderPresent);
  EXPECT_EQ(Result.PnpDevices[1].ParentPDO, Result.PnpDevices[0].PDO);
}

TEST_F(KernelPnpParentLifecycle, SurpriseRemovalDoesNotInventChildTeardown) {
  pnp("parent", DevicePnpRequest::Start);
  pnp("child", DevicePnpRequest::Start);
  reject(begin("parent", DevicePnpRequest::SurpriseRemoval),
         "children to stop first");
  pnp("child", DevicePnpRequest::SurpriseRemoval);
  pnp("parent", DevicePnpRequest::SurpriseRemoval);
  reject(begin("parent", DevicePnpRequest::Remove), "child providers");
  pnp("child", DevicePnpRequest::Remove);
  pnp("parent", DevicePnpRequest::Remove);
}

TEST(KernelPnpTopology, ResolvesForwardParentsWithoutJoiningDeviceStacks) {
  auto Backend = UnicornBackend::create(4 * 1024 * 1024);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  DriverResult Result;
  KernelModel Model(**Backend, Result);
  DriverOptions Options;
  for (const auto *ID : {"child", "root"}) {
    DriverPnpDevice Device;
    Device.ID = ID;
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    Options.PnpDevices.push_back(Device);
  }
  Options.PnpDevices[0].ParentID = "root";
  DriverImage Image;
  Image.Base = 0x180000000;
  Image.Entry = Image.Base + 0x1000;
  Image.Size = 0x3000;
  auto Error = Model.initialize(Image, Options);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  auto Extension =
      (*Backend)->readInteger(Model.driverObject() + DriverExtensionOffset, 8);
  ASSERT_TRUE(bool(Extension)) << llvm::toString(Extension.takeError());
  Error = (*Backend)->writeInteger(*Extension + DriverAddDeviceOffset,
                                   Image.Entry, 8);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Error = Model.finishEntry();
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Error = Model.preparePnpDevices();
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  ASSERT_EQ(Result.PnpDevices.size(), 2u);
  const auto Child = Result.PnpDevices[0];
  const auto Root = Result.PnpDevices[1];
  EXPECT_EQ(Child.ParentID, "root");
  EXPECT_EQ(Child.ParentPDO, Root.PDO);
  EXPECT_FALSE(Root.ParentID);
  EXPECT_FALSE(Root.ParentPDO);
  EXPECT_FALSE(Child.Attached);
  EXPECT_FALSE(Root.Attached);
  auto Call = Model.beginAddDevice("child");
  ASSERT_TRUE(bool(Call)) << llvm::toString(Call.takeError());
  EXPECT_EQ(Call->Argument1, Child.PDO);
  Error = Model.finishAddDevice("child", StatusUnsuccessful);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  EXPECT_FALSE(Result.PnpDevices[0].ProviderPresent);
  EXPECT_EQ(Result.PnpDevices[0].ParentID, "root");
  EXPECT_EQ(Result.PnpDevices[0].ParentPDO, Root.PDO);
  EXPECT_TRUE(Result.PnpDevices[1].ProviderPresent);
  EXPECT_FALSE(Result.PnpDevices[1].Attached);
}
} // namespace
} // namespace neverd::emulation
