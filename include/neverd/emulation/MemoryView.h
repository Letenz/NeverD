//===- MemoryView.h - Retained slices of physical guest allocations -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_MEMORYVIEW_H
#define NEVERD_EMULATION_MEMORYVIEW_H
#include "neverd/emulation/PhysicalMemory.h"

#include "llvm/ADT/ArrayRef.h"

#include <vector>

namespace neverd::emulation {
struct MemorySlice {
  std::shared_ptr<MemoryRegion> Region;
  uint64_t Offset, Size;
};
/// A byte-exact RAM view. Virtual unmapping, address reuse and CPU destruction
/// cannot redirect it. It retains allocations, not an address space or CPU.
class MemoryView final {
public:
  MemoryView() = default;
  uint64_t size() const { return Size; }
  llvm::ArrayRef<MemorySlice> slices() const { return Slices; }
  std::shared_ptr<PhysicalMemory> physicalMemory() const { return Memory; }
  std::shared_ptr<const void> addressSpaceIdentity() const { return Space; }
  llvm::Expected<MemoryView> subview(uint64_t Offset, uint64_t Length) const;
  /// Compare allocation identity and offsets, independent of virtual mappings.
  bool describesSameBytes(const MemoryView &Other) const;
  bool overlaps(const MemoryView &Other) const;
  llvm::Error validateAccess(uint64_t Offset, uint64_t Length) const;
  /// Bypass CPU permissions; require a stopped physical owner and validate the
  /// complete view span before copying. These operations never invoke MMIO.
  llvm::Error read(uint64_t Offset, llvm::MutableArrayRef<uint8_t> Bytes) const;
  llvm::Error write(uint64_t Offset, llvm::ArrayRef<uint8_t> Bytes) const;

private:
  friend class AddressSpace;
  MemoryView(std::shared_ptr<PhysicalMemory> Memory,
             std::shared_ptr<const void> Space, uint64_t Size,
             std::vector<MemorySlice> Slices);
  llvm::Error validate(uint64_t Offset, uint64_t Length) const;
  std::shared_ptr<PhysicalMemory> Memory;
  std::shared_ptr<const void> Space;
  uint64_t Size = 0;
  std::vector<MemorySlice> Slices;
};
} // namespace neverd::emulation
#endif
