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

#include "llvm/ADT/ArrayRef.h"

#include <map>

namespace neverd::emulation {
class KernelFileSystem {
public:
  /// One terminal-provider open, independent of any cloned handles. FileID is
  /// volume-scoped and may be zero. CLEANUP releases sharing; CLOSE retires the
  /// token only after the FILE_OBJECT's final reference is released.
  struct Open {
    uint64_t Token = 0;
    uint64_t FileObject = 0;
    uint64_t DeviceObject = 0;
    uint64_t FileID = 0;
    uint32_t GrantedAccess = 0;
    uint32_t ShareAccess = 0;
    uint32_t SharingDemand = 0;
    bool Cleaned = false;
  };
  struct OpenResult {
    uint32_t Status = 0;
    uint64_t Token = 0;
  };
  struct OpenBinding {
    uint64_t FileObject = 0;
    uint64_t Token = 0;
  };
  struct QueryResult {
    uint32_t Status = 0;
    std::vector<uint8_t> Bytes;
  };

  llvm::Error
  initialize(const std::optional<std::vector<DriverFileSystemVolume>> &Input);
  llvm::Error bindDevice(size_t Volume, uint64_t DeviceObject);
  bool ownsDevice(uint64_t DeviceObject) const;
  const DriverFileSystemVolume *volumeForDevice(uint64_t DeviceObject) const;
  llvm::Expected<const DriverFileSystemNode *>
  lookupNode(uint64_t DeviceObject, llvm::StringRef Path, bool FoldCase) const;
  size_t volumeCount() const { return Volumes ? Volumes->size() : 0; }
  /// Commit sharing after the terminal provider has admitted the name, node
  /// kind, create disposition and access policy. GrantedAccess must already be
  /// mapped by the common object-access owner. This does not perform CREATE,
  /// publish a handle, or establish successful file API support.
  llvm::Expected<OpenResult> reserveOpen(uint64_t DeviceObject, uint64_t FileID,
                                         uint64_t FileObject,
                                         uint32_t GrantedAccess,
                                         uint32_t ShareAccess);
  const Open *findOpen(uint64_t Token) const;
  const DriverFileSystemNode *nodeForOpen(uint64_t Token) const;
  llvm::Expected<QueryResult> queryInformation(uint64_t Token, uint32_t Class,
                                               uint32_t Length) const;
  llvm::Error cleanupOpen(uint64_t Token);
  llvm::Error closeOpen(uint64_t Token);
  size_t openCount() const { return Opens.size(); }
  /// Require an exact one-to-one correspondence with the FILE_OBJECT owner,
  /// including cleaned bodies that still retain a provider token.
  llvm::Error validateOpenBindings(llvm::ArrayRef<OpenBinding> Bindings) const;
  llvm::Expected<std::optional<std::vector<DriverFileSystemVolumeState>>>
  snapshot() const;

private:
  bool Initialized = false;
  std::optional<std::vector<DriverFileSystemVolumeState>> Volumes;
  std::map<uint64_t, Open> Opens;
  uint64_t NextOpen = 1;
};
} // namespace neverd::emulation

#endif
