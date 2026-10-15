//===- KernelFileSystem.h - Session-owned filesystem records --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Own node identity and metadata without duplicating file objects or handles.

#ifndef NEVERD_EMULATION_KERNELFILESYSTEM_H
#define NEVERD_EMULATION_KERNELFILESYSTEM_H

#include "neverd/emulation/DriverFileSystem.h"

namespace neverd::emulation {
class KernelFileSystem {
public:
  llvm::Error
  initialize(const std::optional<std::vector<DriverFileSystemVolume>> &Input);
  llvm::Error bindDevice(size_t Volume, uint64_t DeviceObject);
  bool ownsDevice(uint64_t DeviceObject) const;
  const DriverFileSystemVolume *volumeForDevice(uint64_t DeviceObject) const;
  size_t volumeCount() const { return Volumes ? Volumes->size() : 0; }
  llvm::Expected<std::optional<std::vector<DriverFileSystemVolumeState>>>
  snapshot() const;

private:
  bool Initialized = false;
  std::optional<std::vector<DriverFileSystemVolumeState>> Volumes;
};
} // namespace neverd::emulation

#endif
