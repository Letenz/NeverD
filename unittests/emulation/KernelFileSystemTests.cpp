//===- KernelFileSystemTests.cpp - Filesystem provider identity -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check pre-entry providers, exact teardown and rejection before I/O effects.

#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "neverd/emulation/AddressSpace.h"

#include <algorithm>
#include <tuple>

namespace neverd::emulation {
namespace {
using namespace windows;
constexpr uint64_t ImageBase = 0x180000000, Entry = ImageBase + 0x1000;
constexpr uint64_t Scratch = 0x60000000;
void ok(llvm::Error E) {
  if (E)
    ADD_FAILURE() << llvm::toString(std::move(E));
}
template <class T> T take(llvm::Expected<T> V) {
  if (!V) {
    ADD_FAILURE() << llvm::toString(V.takeError());
    return {};
  }
  return std::move(*V);
}
void rejects(llvm::Error E, llvm::StringRef Fragment) {
  ASSERT_TRUE(bool(E));
  auto Text = llvm::toString(std::move(E));
  EXPECT_NE(Text.find(Fragment.str()), std::string::npos) << Text;
}
template <class T> void rejects(llvm::Expected<T> V, llvm::StringRef Fragment) {
  ASSERT_FALSE(bool(V));
  rejects(V.takeError(), Fragment);
}
DriverOptions options() {
  DriverFileSystemNode Root;
  Root.Path = "\\";
  Root.Kind = DriverFileNodeKind::Directory;
  Root.FileID = 0;
  Root.Attributes = 0x10;
  Root.AllocationSize = 0;
  Root.CreationTime100ns = Root.LastAccessTime100ns = Root.LastWriteTime100ns =
      Root.ChangeTime100ns = 0;
  DriverFileSystemVolume Volume;
  Volume.DeviceName = "\\Device\\DeclaredVolume";
  Volume.AccessPolicy = DriverFileAccessPolicy::FullAccess;
  Volume.SectorSize = 4096;
  Volume.AlignmentMask = 511;
  Volume.Nodes = {Root};
  DriverOptions Options;
  Options.FileSystem = std::vector{Volume};
  return Options;
}
struct Session {
  std::shared_ptr<AddressSpace> Memory = take(AddressSpace::create(
      take(PhysicalMemory::create(16 * 1024 * 1024)), 16 * 1024 * 1024));
  DriverResult Result;
  KernelModel Model{*Memory, Result};
  Session(const DriverOptions &Options = options()) {
    ok(Memory->map(ImageBase, 0x3000, Read | Write));
    ok(Memory->map(Scratch, 0x4000, Read | Write));
    DriverImage Image;
    Image.Base = ImageBase;
    Image.Entry = Entry;
    Image.Size = 0x3000;
    Result.Configuration = Options;
    ok(Model.initialize(Image, Options));
    ok(Model.snapshot());
  }
  uint64_t read(uint64_t Address, unsigned Width = 8) {
    return take(Memory->readInteger(Address, Width));
  }
  void write(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    ok(Model.validateGuestAccess(Address, Width, true));
    ok(Memory->writeInteger(Address, Value, Width));
  }
  uint64_t volume() { return Result.FileSystem->front().DeviceObject; }
  uint64_t device() {
    EXPECT_EQ(take(Model.call("IoCreateDevice", {Model.driverObject(), 0, 0,
                                                 0x22, 0, 0, Scratch})),
              0u);
    return read(Scratch);
  }
  void prepareUnload() {
    write(Model.driverObject() + DriverUnloadOffset, Entry);
    ok(Model.finishEntry());
    take(Model.beginUnload());
  }
};

TEST(KernelFileSystem, PreEntryVolumeHasItsOwnDriverAndDeclaredGeometry) {
  Session S;
  ASSERT_TRUE(S.Result.FileSystem);
  ASSERT_EQ(S.Result.FileSystem->size(), 1u);
  auto Volume = S.volume();
  ASSERT_NE(Volume, 0u);
  EXPECT_TRUE(S.Result.Devices.empty());
  EXPECT_NE(S.read(Volume + DeviceDriverOffset), S.Model.driverObject());
  EXPECT_EQ(S.read(S.Model.driverObject() + DriverDeviceHead), 0u);
  EXPECT_EQ(S.read(Volume + DeviceTypeOffset, 4), 8u);
  EXPECT_EQ(S.read(Volume + DeviceFlagsOffset, 4), 0u);
  EXPECT_EQ(S.read(Volume + DeviceSectorSizeOffset, 2), 4096u);
  EXPECT_EQ(S.read(Volume + DeviceAlignmentOffset, 4), 511u);
  const auto Guest = S.device();
  EXPECT_EQ(take(S.Model.call("IoAttachDeviceToDeviceStack", {Guest, Volume})),
            Volume);
  EXPECT_EQ(S.read(Guest + DeviceAlignmentOffset, 4), 511u);
  EXPECT_EQ(take(S.Model.call("IoGetDeviceAttachmentBaseRef", {Guest})),
            Volume);
  take(S.Model.call("ObfDereferenceObject", {Volume}));
  take(S.Model.call("IoDetachDevice", {Volume}));
  take(S.Model.call("IoDeleteDevice", {Guest}));
  rejects(S.Model.call("IoDeleteDevice", {Volume}), "provider-owned");
  S.prepareUnload();
  ok(S.Model.finishUnload());
  EXPECT_TRUE(S.Result.UnloadCompleted);
  ASSERT_EQ(S.Result.FileSystem->size(), 1u);
  EXPECT_EQ(S.Result.FileSystem->front().DeviceObject, Volume);
}

TEST(KernelFileSystem, ScenarioAndCallerIRPsRejectBeforePublishingFileState) {
  for (bool ThroughFilter : {false, true}) {
    Session S;
    const auto Volume = S.volume();
    uint64_t Target = Volume;
    if (ThroughFilter) {
      Target = S.device();
      EXPECT_EQ(
          take(S.Model.call("IoAttachDeviceToDeviceStack", {Target, Volume})),
          Volume);
    }
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Create;
    Request.File = 1;
    Request.Device = "\\Device\\DeclaredVolume";
    const auto Before = S.Model.unpackAllocations();
    rejects(S.Model.beginRequest(Request), "not implemented");
    EXPECT_EQ(S.Model.unpackAllocations(), Before);
    ASSERT_EQ(S.Result.Requests.size(), 1u);
    EXPECT_EQ(S.Result.Requests.front().IRP, 0u);
    const auto IRP = take(S.Model.call("IoAllocateIrp", {3, 0}));
    ASSERT_NE(IRP, 0u);
    std::vector<uint8_t> Bytes(IRPSize + 3 * StackSize), After(Bytes.size());
    ok(S.Memory->snapshotBacking(IRP, Bytes));
    const auto RequestCount = S.Result.Requests.size();
    rejects(S.Model.call("IofCallDriver", {Target, IRP}), "not implemented");
    ok(S.Memory->snapshotBacking(IRP, After));
    EXPECT_EQ(Bytes, After);
    EXPECT_EQ(S.Result.Requests.size(), RequestCount);
    take(S.Model.call("IoFreeIrp", {IRP}));
  }
}

TEST(KernelFileSystem, UnloadStillRejectsReferencesGuestDevicesAndAttachments) {
  for (unsigned Leak = 0; Leak != 3; ++Leak) {
    Session S;
    if (Leak == 0)
      take(S.Model.call("IoGetAttachedDeviceReference", {S.volume()}));
    else {
      const auto Guest = S.device();
      if (Leak == 2)
        take(S.Model.call("IoAttachDeviceToDeviceStack", {Guest, S.volume()}));
    }
    S.prepareUnload();
    rejects(S.Model.finishUnload(), Leak ? "live devices" : "references");
    EXPECT_FALSE(S.Result.UnloadCompleted);
  }
}

TEST(KernelFileSystem, ProtectedProjectionAndSnapshotDetectDivergence) {
  for (const auto [Offset, Width, Value] :
       {std::tuple{DeviceAlignmentOffset, 4u, 63u},
        std::tuple{DeviceSectorSizeOffset, 2u, 512u},
        std::tuple{DeviceFlagsOffset, 4u, 0x80u}}) {
    Session S;
    rejects(S.Model.validateGuestAccess(S.volume() + Offset, Width, true),
            "opaque");
    ok(S.Memory->writeInteger(S.volume() + Offset, Value, Width));
    S.Result.FileSystem.reset();
    rejects(S.Model.snapshot(), "volume device");
    ASSERT_TRUE(S.Result.FileSystem);
    EXPECT_EQ(S.Result.FileSystem->front().Volume.AlignmentMask, 511u);
    EXPECT_EQ(S.Result.FileSystem->front().Volume.SectorSize, 4096u);
  }
}

TEST(KernelFileSystem, VolumePointerDoesNotInventDeviceHandleSupport) {
  Session S;
  S.write(Scratch, 0x1122334455667788);
  rejects(S.Model.call("ObOpenObjectByPointer",
                       {S.volume(), 0, 0, 0, 0, KernelMode, Scratch}),
          "unsupported object handle class");
  EXPECT_EQ(S.read(Scratch), 0x1122334455667788u);
  S.prepareUnload();
  ok(S.Model.finishUnload());
}

TEST(KernelFileSystem, CorruptedProviderDriverStillRetainsOwnerState) {
  for (uint64_t Offset : {uint64_t(0), uint64_t(2), DriverDeviceHead}) {
    Session S;
    const auto Device = S.volume();
    const auto Owner = S.read(Device + DeviceDriverOffset);
    const unsigned Width = Offset == DriverDeviceHead ? 8 : 2;
    rejects(S.Model.validateGuestAccess(Owner + Offset, Width, true), "opaque");
    ok(S.Memory->writeInteger(Owner + Offset, 0, Width));
    S.Result.FileSystem.reset();
    rejects(S.Model.snapshot(),
            Offset == DriverDeviceHead ? "device list" : "volume driver");
    ASSERT_TRUE(S.Result.FileSystem);
    ASSERT_EQ(S.Result.FileSystem->size(), 1u);
    EXPECT_EQ(S.Result.FileSystem->front().DeviceObject, Device);
    EXPECT_EQ(S.Result.FileSystem->front().Volume.DeviceName,
              "\\Device\\DeclaredVolume");
  }
}

TEST(KernelFileSystem, PnpAndVolumeDriversHaveIndependentOwnership) {
  auto Options = options();
  Options.FileSystem->push_back(Options.FileSystem->front());
  Options.FileSystem->back().DeviceName = "\\Device\\SecondVolume";
  DriverPnpDevice Pnp;
  Pnp.ID = "bus";
  Pnp.InitialDevicePower = DevicePowerState::D0;
  Pnp.InitialSystemPower = SystemPowerState::Working;
  Options.PnpDevices.push_back(Pnp);
  Session S(Options);
  ASSERT_EQ(S.Result.FileSystem->size(), 2u);
  const auto A = S.volume(), B = S.Result.FileSystem->back().DeviceObject;
  const auto Owner = S.read(A + DeviceDriverOffset);
  EXPECT_EQ(S.read(B + DeviceDriverOffset), Owner);
  EXPECT_EQ(S.read(Owner + DriverDeviceHead), B);
  EXPECT_EQ(S.read(B + DeviceNext), A);
  S.write(S.read(S.Model.driverObject() + DriverExtensionOffset) +
              DriverAddDeviceOffset,
          Entry);
  S.write(S.Model.driverObject() + DriverUnloadOffset, Entry);
  ok(S.Model.finishEntry());
  ok(S.Model.preparePnpDevices());
  ASSERT_EQ(S.Result.PnpDevices.size(), 1u);
  const auto PDO = S.Result.PnpDevices.front().PDO;
  EXPECT_NE(S.read(PDO + DeviceDriverOffset), Owner);
  EXPECT_NE(S.read(PDO + DeviceDriverOffset), S.Model.driverObject());
  EXPECT_TRUE(S.read(PDO + DeviceFlagsOffset, 4) & DeviceBusEnumerated);
  EXPECT_FALSE(S.read(A + DeviceFlagsOffset, 4) & DeviceBusEnumerated);
  rejects(S.Model.validateGuestAccess(Owner + DriverDeviceHead, 8, true),
          "opaque");
  rejects(S.Model.validateGuestAccess(
              S.read(PDO + DeviceDriverOffset) + DriverDeviceHead, 8, true),
          "opaque");
  rejects(S.Model.beginUnload(), "retire");
  take(S.Model.beginAddDevice("bus"));
  ok(S.Model.finishAddDevice("bus", StatusNotSupported));
  ok(S.Model.snapshot());
  EXPECT_FALSE(S.Result.PnpDevices.front().ProviderPresent);
  ASSERT_EQ(S.Result.FileSystem->size(), 2u);
  EXPECT_EQ(S.Result.FileSystem->front().DeviceObject, A);
  EXPECT_TRUE(S.Result.Devices.empty());
  take(S.Model.beginUnload());
  ok(S.Model.finishUnload());
}

TEST(KernelFileSystem, UnnamedRequestSelectionCountsOnlyLiveGuestDevices) {
  Session S;
  S.write(S.Model.driverObject() + DriverDispatchOffset, Entry);
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Create;
  Request.File = 1;
  rejects(S.Model.beginRequest(Request), "one live guest device");
  const auto Guest = S.device();
  S.write(Guest + DeviceFlagsOffset, 0, 4);
  const auto Invocation = take(S.Model.beginRequest(Request));
  ASSERT_NE(Invocation.IRP, 0u);
  EXPECT_EQ(Invocation.Argument0, Guest);
  EXPECT_EQ(Invocation.PC, Entry);
  EXPECT_EQ(S.Result.Requests.back().IRP, Invocation.IRP);
  EXPECT_EQ(S.read(Invocation.IRP + IRPOriginalFileOffset) != 0, true);
}

TEST(KernelFileSystem, OmittedInventoryDoesNotAcquireAnUnpackDependency) {
  Session S(DriverOptions{});
  ASSERT_FALSE(S.Result.FileSystem);
  ok(S.Model.captureUnpackBaseline());
  const auto Reasons = take(S.Model.unpackDependencies());
  EXPECT_EQ(std::find(Reasons.begin(), Reasons.end(),
                      "unreconstructed kernel or environment effects"),
            Reasons.end());
}

TEST(KernelFileSystem, EvenUnusedEmptyDeclarationRemainsAnUnpackDependency) {
  for (bool Empty : {false, true}) {
    auto Options = options();
    if (Empty)
      Options.FileSystem->clear();
    Session S(Options);
    ok(S.Model.captureUnpackBaseline());
    const auto Reasons = take(S.Model.unpackDependencies());
    EXPECT_NE(std::find(Reasons.begin(), Reasons.end(),
                        "unreconstructed kernel or environment effects"),
              Reasons.end());
    ok(S.Model.snapshot());
    EXPECT_EQ(take(S.Model.unpackDependencies()), Reasons);
  }
}
} // namespace
} // namespace neverd::emulation
