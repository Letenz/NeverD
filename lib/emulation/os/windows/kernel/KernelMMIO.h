//===- KernelMMIO.h - Resource epochs and physical register banks ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Own explicit synthetic physical registers independently of virtual aliases,
/// top-level PnP state and driver power notifications.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELMMIO_H
#define NEVERD_EMULATION_KERNELMMIO_H

#include "KernelResources.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/DriverPnp.h"
#include "neverd/emulation/GuestMemory.h"

#include <map>
#include <memory>
#include <vector>

namespace neverd::emulation {
namespace mmio {
#define NEVERD_KERNEL_MMIO_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "KernelMMIOValues.def"
#undef NEVERD_KERNEL_MMIO_VALUE
} // namespace mmio

class KernelMMIO {
public:
  KernelMMIO(GuestMemory &Memory, const KernelResources &Resources);
  ~KernelMMIO();
  KernelMMIO(const KernelMMIO &) = delete;
  KernelMMIO &operator=(const KernelMMIO &) = delete;
  /// Seed mutable register values from the immutable assignment inventory.
  llvm::Error configure(uint64_t PDO);
  llvm::Error canRemove(uint64_t PDO) const;
  llvm::Expected<uint64_t> map(uint64_t Physical, uint64_t Length,
                               uint32_t Attributes, bool Extended);
  llvm::Error unmap(uint64_t Address, uint64_t Length);

private:
  struct Device {
    std::vector<DriverMemoryResource> Resources;
    uint64_t PowerGeneration = 0;
    std::shared_ptr<const void> Revision = std::make_shared<unsigned char>(0);
  };
  struct Mapping {
    uint64_t PDO;
    size_t ResourceIndex;
    uint64_t Epoch;
    uint64_t Address;
    uint64_t Length;
    uint64_t PageBase;
    uint64_t PageSize;
    uint64_t Physical;
    bool ReadOnly;
  };
  GuestMemory &Memory;
  const KernelResources &Resources;
  struct CallbackState {
    std::shared_ptr<std::recursive_mutex> Mutex;
    KernelMMIO *Owner;
  };
  std::shared_ptr<CallbackState> State;
  std::map<uint64_t, Device> Devices;
  std::map<uint64_t, Mapping> Mappings;
  uint64_t NextMapping = 0;
  void restorePowerContext(uint64_t PDO);
  GuestMMIOCallbacks callbacks(uint64_t Address);
  llvm::Expected<GuestMMIOPreparedAtomic>
  prepare(uint64_t Address, uint64_t Offset, unsigned Size, bool Write);
  llvm::Error validate(uint64_t Address, uint64_t Offset, uint64_t Size,
                       bool Write) const;
  llvm::Expected<uint64_t> read(uint64_t Address, uint64_t Offset,
                                unsigned Size);
  llvm::Expected<uint64_t> peek(uint64_t Address, uint64_t Offset,
                                unsigned Size) const;
  llvm::Error write(uint64_t Address, uint64_t Offset, unsigned Size,
                    uint64_t Value);
};

} // namespace neverd::emulation
#endif // NEVERD_EMULATION_KERNELMMIO_H
