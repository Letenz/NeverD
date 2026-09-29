//===- AddressSpace.h - Guest virtual mappings ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ADDRESSSPACE_H
#define NEVERD_EMULATION_ADDRESSSPACE_H
#include "neverd/emulation/BackendFault.h"
#include "neverd/emulation/GuestMemory.h"
#include "neverd/emulation/PhysicalMemory.h"

#include <memory>

namespace neverd::emulation {
/// One guest virtual-address namespace over a shared physical owner.
/// Permissions and alias budgets belong to this space. Physical allocation
/// handles can outlive their original mapping and can be mapped into siblings.
class AddressSpace final : public GuestMemory {
public:
  static llvm::Expected<std::shared_ptr<AddressSpace>>
  create(std::shared_ptr<PhysicalMemory> Memory, uint64_t MappingLimit);
  ~AddressSpace() override;
  std::shared_ptr<PhysicalMemory> physicalMemory() const;
  uint64_t mappedBytes() const;
  uint64_t mappingGeneration() const;

  /// Map part of an existing allocation from this space's physical owner.
  llvm::Error mapRegion(uint64_t Address, std::shared_ptr<MemoryRegion> Region,
                        uint64_t Offset, uint64_t Size, unsigned Permissions);
  /// Retire a mapped, page-aligned range. Other mappings and retained
  /// allocation handles survive. Partial alias removal leaves exact surviving
  /// fragments.
  llvm::Error unmap(uint64_t Address, uint64_t Size);
  llvm::Error map(uint64_t, uint64_t, unsigned) override;
  llvm::Error mapAlias(uint64_t, uint64_t, uint64_t, unsigned) override;
  llvm::Error unmapAlias(uint64_t, uint64_t) override;
  llvm::Error replaceAliases(llvm::ArrayRef<GuestAliasRange>,
                             llvm::ArrayRef<GuestAliasMapping>) override;
  llvm::Error protect(uint64_t, uint64_t, unsigned) override;
  /// Register device metadata. Device transactions require a capable CPU;
  /// direct address-space reads/writes and backing operations access RAM only.
  llvm::Error mapMMIO(uint64_t, uint64_t, GuestMMIOCallbacks) override;
  llvm::Error unmapMMIO(uint64_t, uint64_t) override;
  llvm::Error read(uint64_t, llvm::MutableArrayRef<uint8_t>) override;
  llvm::Error write(uint64_t, llvm::ArrayRef<uint8_t>) override;
  llvm::Expected<bool> canAccess(uint64_t, uint64_t, unsigned) const override;
  llvm::Error validateBacking(uint64_t, uint64_t) const override;
  llvm::Error readBacking(uint64_t, llvm::MutableArrayRef<uint8_t>) override;
  llvm::Error writeBacking(uint64_t, llvm::ArrayRef<uint8_t>) override;
  llvm::Error snapshotBacking(uint64_t,
                              llvm::MutableArrayRef<uint8_t>) override;

private:
  friend class MemoryProjection;
  struct Impl;
  explicit AddressSpace(std::unique_ptr<Impl> State);
  llvm::Error readWithPermissions(uint64_t, llvm::MutableArrayRef<uint8_t>,
                                  unsigned) const;
  llvm::Error writeWithPermissions(uint64_t, llvm::ArrayRef<uint8_t>, unsigned);
  std::unique_ptr<Impl> State;
};
} // namespace neverd::emulation
#endif
