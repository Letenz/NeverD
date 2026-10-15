//===- KernelModelFileSystem.cpp - Canonical filesystem volume devices ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Install declared terminal providers without fabricating file I/O results.

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <array>

namespace neverd::emulation {
namespace {
using namespace windows;
constexpr uint32_t DiskFileSystemDeviceType = 8;
llvm::Error filesystemError(const llvm::Twine &Text) {
  return llvm::createStringError("filesystem provider: " + Text);
}
} // namespace

uint64_t KernelModel::deviceOwnerDriver(DeviceOwnerKind Kind) const {
  switch (Kind) {
  case DeviceOwnerKind::Guest:
    return DriverObject;
  case DeviceOwnerKind::PnpProvider:
    return PnpProviderDriver;
  case DeviceOwnerKind::FileSystemProvider:
    return FileSystemProviderDriver;
  }
  return 0;
}

llvm::Error KernelModel::initializeFileSystem(const DriverOptions &Options) {
  if (auto E = FileSystem.initialize(Options.FileSystem))
    return E;
  if (!Options.FileSystem)
    return llvm::Error::success();
  // Pre-entry devices enter the recovery baseline. Unchanged bytes or an unused
  // declaration cannot make this unreconstructed environment a native fact.
  UnpackOpaqueEffects = true;
  if (Options.FileSystem->empty())
    return llvm::Error::success();
  auto Driver = allocate(DriverObjectSize);
  if (!Driver)
    return Driver.takeError();
  if (auto E = Memory.writeInteger(*Driver, DriverType, 2))
    return E;
  if (auto E = Memory.writeInteger(*Driver + 2, DriverObjectSize, 2))
    return E;
  FileSystemProviderDriver = *Driver;
  ReferencedObjects.emplace(*Driver, ObjectReferences{ObjectKind::Driver});
  for (size_t I = 0; I < Options.FileSystem->size(); ++I) {
    const auto &Volume = (*Options.FileSystem)[I];
    auto Device = createDeviceObjectForOwner(
        Volume.DeviceName, 0, DiskFileSystemDeviceType, SecureOpen, false,
        FileSystemProviderDriver, DeviceOwnerKind::FileSystemProvider);
    if (!Device)
      return Device.takeError();
    if (Device->Status != StatusSuccess)
      return filesystemError("declared volume device could not be installed");
    if (auto E = Memory.writeInteger(Device->Address + DeviceAlignmentOffset,
                                     *Volume.AlignmentMask, 4))
      return E;
    if (auto E = Memory.writeInteger(Device->Address + DeviceSectorSizeOffset,
                                     *Volume.SectorSize, 2))
      return E;
    if (auto E = FileSystem.bindDevice(I, Device->Address))
      return E;
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::validateFileSystemDevice(uint64_t Address,
                                                  ObjectRead Mode) const {
  const auto *Volume = FileSystem.volumeForDevice(Address);
  const auto Found = Devices.find(Address);
  if (!Volume || Found == Devices.end() ||
      Found->second.OwnerKind != DeviceOwnerKind::FileSystemProvider ||
      Found->second.OwnerDriver != FileSystemProviderDriver ||
      Found->second.Name != Volume->DeviceName)
    return filesystemError("volume lost its canonical device identity");
  for (const auto [Offset, Expected] :
       {std::pair{uint64_t(0), uint64_t(DriverType)},
        std::pair{uint64_t(2), uint64_t(DriverObjectSize)}}) {
    auto Value = readObjectField(FileSystemProviderDriver + Offset, 2, Mode);
    if (!Value)
      return Value.takeError();
    if (*Value != Expected)
      return filesystemError("volume driver identity differs from its owner");
  }
  struct Field {
    uint64_t Offset, Value;
    unsigned Width;
  };
  for (const auto &F : std::array<Field, 6>{
           {{DeviceTypeOffset, DiskFileSystemDeviceType, 4},
            {DeviceFlagsOffset, 0, 4},
            {DeviceCharacteristicsOffset, SecureOpen, 4},
            {DeviceVPBOffset, 0, 8},
            {DeviceAlignmentOffset, *Volume->AlignmentMask, 4},
            {DeviceSectorSizeOffset, *Volume->SectorSize, 2}}}) {
    auto Value = readObjectField(Address + F.Offset, F.Width, Mode);
    if (!Value)
      return Value.takeError();
    if (*Value != F.Value)
      return filesystemError("volume device fields differ from their owner");
  }
  return llvm::Error::success();
}

llvm::Error
KernelModel::validateFileSystemRoute(llvm::ArrayRef<uint64_t> Route) const {
  for (uint64_t Address : Route) {
    const auto Found = Devices.find(Address);
    if (Found == Devices.end())
      return filesystemError("captured route lost its device identity");
    if (Found->second.OwnerKind == DeviceOwnerKind::FileSystemProvider)
      return filesystemError(
          "file I/O for a declared volume is not implemented");
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
