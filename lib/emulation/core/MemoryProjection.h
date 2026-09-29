//===- MemoryProjection.h - Per-CPU projection over shared RAM ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_CORE_MEMORYPROJECTION_H
#define NEVERD_EMULATION_CORE_MEMORYPROJECTION_H
#include "MemoryStorage.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <array>

namespace neverd::emulation {
struct MemoryRegistration {
  uint64_t Physical;
  uint8_t *Backing;
  uint64_t Size;
};
/// Owns only private monitor/page-table storage. AddressSpace is the mapping
/// authority; physical allocations are shared and retained by every projection.
class MemoryProjection {
public:
  using Page = AddressSpace::Impl::Page;
  using Device = AddressSpace::Impl::Device;
  static llvm::Expected<std::unique_ptr<MemoryProjection>>
  create(uint64_t Limit);
  static llvm::Expected<std::unique_ptr<MemoryProjection>>
  create(std::shared_ptr<AddressSpace> Space);
  ~MemoryProjection();
  std::shared_ptr<AddressSpace> addressSpace() const { return Space; }
  llvm::Expected<std::unique_lock<std::recursive_mutex>> lock() const;
  llvm::Error mutableMemory() const;
  llvm::Error
  validateMappings(llvm::function_ref<bool(uint64_t, uint64_t)> Valid,
                   bool AllowDevices = false) const;
  llvm::Error bind(std::shared_ptr<AddressSpace> Next,
                   llvm::function_ref<bool(uint64_t, uint64_t)> Valid,
                   bool AllowDevices = false);
  llvm::Error beginRun();
  void endRun();
  llvm::Error map(uint64_t A, uint64_t N, unsigned P) {
    return Space->map(A, N, P);
  }
  llvm::Error aliases(llvm::ArrayRef<GuestAliasRange> Remove,
                      llvm::ArrayRef<GuestAliasMapping> Add) {
    return Space->replaceAliases(Remove, Add);
  }
  llvm::Error protect(uint64_t A, uint64_t N, unsigned P) {
    return Space->protect(A, N, P);
  }
  std::optional<BackendFaultKind> check(uint64_t A, uint64_t N,
                                        unsigned P) const;
  llvm::Error read(uint64_t A, llvm::MutableArrayRef<uint8_t> B,
                   unsigned P = Read) const {
    return Space->readWithPermissions(A, B, P);
  }
  llvm::Error write(uint64_t A, llvm::ArrayRef<uint8_t> B, unsigned P = Write) {
    return Space->writeWithPermissions(A, B, P);
  }
  const std::map<uint64_t, Page> &mappings() const {
    return Space->State->Pages;
  }
  uint64_t mappingGeneration() const { return Space->mappingGeneration(); }
  bool needsProjection() const {
    return ProjectedSpace.lock() != Space || Generation != mappingGeneration();
  }
  const std::map<uint64_t, std::shared_ptr<Device>> &devices() const {
    return Space->State->Devices;
  }
  void commitProjection();
  uint8_t *data() const { return static_cast<uint8_t *>(Projection.base()); }
  std::array<MemoryRegistration, 2> registrations() const;
  uint8_t *physicalPointer(uint64_t GPA) const;

private:
  MemoryProjection(std::shared_ptr<AddressSpace> Space,
                   llvm::sys::MemoryBlock Projection);
  std::shared_ptr<AddressSpace> Space;
  llvm::sys::MemoryBlock Projection;
  uint64_t Generation = 0;
  std::weak_ptr<AddressSpace> ProjectedSpace;
  // Keep old allocations pinned until the transport retires the old mapping.
  std::map<uint64_t, Page> ProjectedPages;
};
} // namespace neverd::emulation
#endif
