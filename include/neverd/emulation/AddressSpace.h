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
/// Complete initial bytes and final permissions of one new RAM range.
struct InitializedMapping {
  uint64_t Address;
  unsigned Permissions;
  llvm::ArrayRef<uint8_t> Bytes;
};
/// Trusted environment initialization, independent of guest write permission.
/// Patches target ordinary RAM and preserve its allocation/alias identity.
struct BackingPatch {
  uint64_t Address;
  llvm::ArrayRef<uint8_t> Bytes;
};
struct AddressMapping {
  uint64_t Address, Size;
  unsigned Permissions;
  bool Device;
};
/// One guest virtual-address namespace over a shared physical owner.
/// Permissions and alias budgets belong to this space. Physical allocation
/// handles can outlive their original mapping and can be mapped into siblings.
class AddressSpace final : public GuestMemory,
                           public std::enable_shared_from_this<AddressSpace> {
public:
  /// Prepared additions retain the physical owner's stopped lease. Existing
  /// mappings and bytes remain visible until commit. Destruction without
  /// commit releases all reservations, preserving the mapping generation.
  /// Keep this object on its creating thread, do not nest transactions, and
  /// finish publication of related metadata before releasing it. CPU execution
  /// and memory mutation reject this lease, including on the creating thread.
  class MappingTransaction final {
  public:
    ~MappingTransaction();
    MappingTransaction(const MappingTransaction &) = delete;
    MappingTransaction &operator=(const MappingTransaction &) = delete;
    /// Publish once; repeated commit is rejected without effects. Preparation
    /// completed every allocation and validation of the mapping batch.
    llvm::Error commit();

  private:
    friend class AddressSpace;
    struct Impl;
    explicit MappingTransaction(std::unique_ptr<Impl> State);
    std::unique_ptr<Impl> State;
  };

  /// Stage new page-aligned RAM ranges and/or bounded backing patches. Every
  /// patch byte must have RAM in the resulting layout. Physically overlapping
  /// patches, including through aliases, are rejected. Preparation copies all
  /// inputs; abandonment preserves existing bytes and reservation witnesses.
  /// This does not touch backend projections. Their normal generation check
  /// synchronizes before execution; projection failure remains terminal.
  llvm::Expected<std::unique_ptr<MappingTransaction>>
  prepareMappings(llvm::ArrayRef<InitializedMapping> Mappings,
                  llvm::ArrayRef<BackingPatch> Patches = {});
  /// Retain the same stopped lease for metadata over existing RAM. Commit
  /// changes no mappings, bytes, reservation witnesses or mapping generation.
  /// This does not make an empty prepareMappings batch valid.
  llvm::Expected<std::unique_ptr<MappingTransaction>>
  prepareMetadataPublication();
  static llvm::Expected<std::shared_ptr<AddressSpace>>
  create(std::shared_ptr<PhysicalMemory> Memory, uint64_t MappingLimit);
  ~AddressSpace() override;
  std::shared_ptr<AddressSpace> addressSpace() const override;
  std::shared_ptr<const void> identity() const;
  /// Allocate zeroed, page-sized RAM without publishing a virtual mapping.
  /// The returned view retains this space's physical owner and identity;
  /// allocation capacity is returned when its final view/mapping is released.
  llvm::Expected<MemoryView> allocateBacking(uint64_t Size);
  llvm::Expected<MemoryView> pinBacking(uint64_t, uint64_t) const override;
  llvm::Error validatePinned(const MemoryView &, uint64_t,
                             uint64_t) const override;
  llvm::Error readPinned(const MemoryView &, uint64_t,
                         llvm::MutableArrayRef<uint8_t>) override;
  llvm::Error writePinned(const MemoryView &, uint64_t,
                          llvm::ArrayRef<uint8_t>) override;
  std::shared_ptr<PhysicalMemory> physicalMemory() const;
  uint64_t mappedBytes() const;
  uint64_t mappingGeneration() const;
  /// Point-in-time virtual layout, sorted and coalesced by permissions and
  /// RAM/device kind. This query does not pin allocations or preserve mapping
  /// identities. Callers must synchronize later mutations independently.
  llvm::Expected<std::vector<AddressMapping>> mappings() const;

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
