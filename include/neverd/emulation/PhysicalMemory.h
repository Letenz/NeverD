//===- PhysicalMemory.h - Shared guest RAM ownership ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PHYSICALMEMORY_H
#define NEVERD_EMULATION_PHYSICALMEMORY_H
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>

namespace neverd::emulation {
class AddressSpace;
class MemoryProjection;
class MemoryRegion;

/// Physical RAM is independent of address spaces, CPUs and their transports.
/// Regions and mappings keep their owner alive. Releasing the final reference
/// to a region returns its capacity to this owner, without affecting other RAM.
class PhysicalMemory final
    : public std::enable_shared_from_this<PhysicalMemory> {
public:
  static llvm::Expected<std::shared_ptr<PhysicalMemory>> create(uint64_t Limit);
  ~PhysicalMemory();
  llvm::Expected<std::shared_ptr<MemoryRegion>> allocate(uint64_t Size);
  uint64_t limit() const;
  uint64_t allocatedBytes() const;

private:
  friend class AddressSpace;
  friend class MemoryProjection;
  friend class MemoryRegion;
  struct Impl;
  explicit PhysicalMemory(std::unique_ptr<Impl> State);
  std::unique_ptr<Impl> State;
};

/// An allocation identity, not a virtual address or a CPU-context snapshot.
/// Multiple address spaces may map overlapping portions of the same region.
class MemoryRegion final {
public:
  ~MemoryRegion();
  MemoryRegion(const MemoryRegion &) = delete;
  MemoryRegion &operator=(const MemoryRegion &) = delete;
  uint64_t size() const { return Size; }

private:
  friend class PhysicalMemory;
  friend class AddressSpace;
  friend class MemoryProjection;
  MemoryRegion(std::shared_ptr<PhysicalMemory> Owner, uint64_t Offset,
               uint64_t Size);
  std::shared_ptr<PhysicalMemory> Owner;
  uint64_t Offset, Size;
};
} // namespace neverd::emulation
#endif
