//===- MemoryStorage.h - Internal RAM and mapping authorities -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_CORE_MEMORYSTORAGE_H
#define NEVERD_EMULATION_CORE_MEMORYSTORAGE_H
#include "neverd/emulation/AddressSpace.h"

#include "llvm/Support/Memory.h"

#include <atomic>
#include <map>
#include <mutex>

namespace neverd::emulation {
/// A retained physical interval and its write witness. All validity changes
/// are serialized by the physical owner's execution mutex. Keeping the view
/// alive prevents allocation reuse from making an old reservation match.
struct RAMReservation {
  MemoryView Bytes;
  uint64_t Granule, GranuleSize;
  bool Valid = true;
};
struct PhysicalMemory::Impl {
  llvm::sys::MemoryBlock Backing;
  uint64_t Limit = 0;
  std::atomic<uint64_t> Used{0};
  std::map<uint64_t, uint64_t> Free;
  mutable std::mutex AllocatorMutex;
  // A checked CPU holds this lease across decode, preflight and execution.
  // Same-thread read-only observers may inspect memory through recursive locks;
  // mutations and recursive CPU execution still reject the Running state.
  mutable std::recursive_mutex Mutex;
  bool Running = false;
  std::vector<std::weak_ptr<RAMReservation>> Reservations;
  // Called only for committed guest writes, including identical-value stores.
  // Neither invalidation operation allocates or invokes observers.
  void invalidateReservations(uint64_t Physical, uint64_t Size);
  void invalidateReservations();
};
struct MemoryAccessFailure {
  BackendFaultKind Kind;
  uint64_t Address, Size;
};
struct AddressSpace::Impl {
  struct Device {
    uint64_t Address, Size;
    GuestMMIOCallbacks Callbacks;
  };
  struct Page {
    uint64_t Physical;
    unsigned Permissions;
    std::shared_ptr<MemoryRegion> Region;
    std::shared_ptr<Device> IO;
  };
  std::shared_ptr<PhysicalMemory> Memory;
  std::shared_ptr<const void> Identity = std::make_shared<unsigned char>(0);
  uint64_t Limit = 0;
  std::atomic<uint64_t> Used{0}, Generation{1};
  std::map<uint64_t, Page> Pages;
  std::map<uint64_t, uint64_t> Aliases;
  std::map<uint64_t, std::shared_ptr<Device>> Devices;
  bool overlapsDevice(uint64_t Address, uint64_t Size) const;
  std::optional<MemoryAccessFailure>
  firstAccessFailure(uint64_t Address, uint64_t Size,
                     unsigned Permissions) const;
  std::optional<BackendFaultKind> check(uint64_t Address, uint64_t Size,
                                        unsigned Permissions) const;
};
} // namespace neverd::emulation
#endif
