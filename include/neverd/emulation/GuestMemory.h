//===- GuestMemory.h - Guest memory interface -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest memory interface.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_PUBLIC_GUESTMEMORY_H
#define NEVERD_EMULATION_PUBLIC_GUESTMEMORY_H

#include "neverd/emulation/MemoryView.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace neverd::emulation {
class AddressSpace;
/// RWX applies to trusted host/supervisor access. User CPUs additionally
/// require UserAccessible on every page; the bit alone grants no
/// read/write/execute right. Flat software contracts ignore privilege and apply
/// only RWX.
enum GuestPermission : unsigned {
#define NEVERD_GUEST_PERMISSION(Name, Value) Name = Value,
#include "neverd/emulation/GuestPermissions.def"
#undef NEVERD_GUEST_PERMISSION
};
#define NEVERD_GUEST_PERMISSION_MASK(Name, Value)                              \
  inline constexpr unsigned Name = Value;
#include "neverd/emulation/GuestPermissions.def"
#undef NEVERD_GUEST_PERMISSION_MASK
/// A mapping cannot fit in the configured guest memory budget. Callers may
/// translate this specific shortage into their documented allocation result.
class GuestMemoryLimitError : public llvm::ErrorInfo<GuestMemoryLimitError> {
public:
  static char ID;
  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;
};
/// One device mapping. Validate is pure and receives the original transaction
/// before any read/write effect. Offsets are relative to the mapped page base.
/// Callbacks are shared by CPU contexts and live until successful unmap.
struct GuestMMIOCallbacks {
  std::function<llvm::Error(uint64_t, uint64_t, bool)> Validate;
  std::function<llvm::Expected<uint64_t>(uint64_t, unsigned)> Read;
  std::function<llvm::Error(uint64_t, unsigned, uint64_t)> Write;
};
struct GuestAliasRange {
  uint64_t Address, Size;
};
struct GuestAliasMapping {
  uint64_t Address, Source, Size;
  unsigned Permissions;
};
class GuestMemory {
public:
  virtual ~GuestMemory() = default;
  /// Mapping authority when this provider supports independently owned RAM.
  virtual std::shared_ptr<AddressSpace> addressSpace() const { return {}; }
  /// Capture allocation slices without preserving the source VA mapping.
  virtual llvm::Expected<MemoryView> pinBacking(uint64_t Address,
                                                uint64_t Size) const;
  /// Preflight the complete retained span without looking up its original VA.
  virtual llvm::Error validatePinned(const MemoryView &View, uint64_t Offset,
                                     uint64_t Size) const;
  virtual llvm::Error readPinned(const MemoryView &View, uint64_t Offset,
                                 llvm::MutableArrayRef<uint8_t> Bytes);
  virtual llvm::Error writePinned(const MemoryView &View, uint64_t Offset,
                                  llvm::ArrayRef<uint8_t> Bytes);
  virtual llvm::Error map(uint64_t Address, uint64_t Size,
                          unsigned Permissions) = 0;
  /// Map a second virtual range onto the same RAM pages. The source and alias
  /// are page aligned; the owner must separately govern their lifetimes.
  virtual llvm::Error mapAlias(uint64_t Address, uint64_t Source, uint64_t Size,
                               unsigned Permissions);
  /// Retire exactly one complete RAM alias. Canonical storage and other
  /// aliases survive; the address and mapping budget become available again.
  /// Reject running/faulted CPUs, callbacks and partial or canonical ranges.
  virtual llvm::Error unmapAlias(uint64_t Address, uint64_t Size);
  /// Replace a set of complete RAM aliases at one stopped-CPU boundary.
  /// Validate every removal, source, destination and the final mapping budget
  /// before any change. Sources must remain mapped throughout the operation.
  /// An unexpected engine failure is terminal; predictable errors leave all
  /// aliases, permissions and accounting unchanged.
  virtual llvm::Error replaceAliases(llvm::ArrayRef<GuestAliasRange> Remove,
                                     llvm::ArrayRef<GuestAliasMapping> Add);
  virtual llvm::Error protect(uint64_t Address, uint64_t Size,
                              unsigned Permissions) = 0;
  /// Optional device access support; unrelated memory implementations reject
  /// it. Address and Size describe whole nonempty pages. Device pages are NX.
  virtual llvm::Error mapMMIO(uint64_t Address, uint64_t Size,
                              GuestMMIOCallbacks Callbacks);
  /// Retire exactly one complete device mapping, including its callbacks.
  virtual llvm::Error unmapMMIO(uint64_t Address, uint64_t Size);
  virtual llvm::Error read(uint64_t Address,
                           llvm::MutableArrayRef<uint8_t> Bytes) = 0;
  virtual llvm::Error write(uint64_t Address,
                            llvm::ArrayRef<uint8_t> Bytes) = 0;
  /// Pure whole-range validation for device access to existing RAM backing.
  /// This bypasses CPU permissions, never MMIO or mapping/lifetime checks.
  /// The model must separately authorize the exact live allocation and pins.
  virtual llvm::Error validateBacking(uint64_t Address, uint64_t Size) const;
  /// Pure CPU-permission preflight. False means an access would fault; no
  /// first-fault state may be latched by this query. Instruction and memory
  /// hooks may query permissions without reentering the execution engine.
  virtual llvm::Expected<bool> canAccess(uint64_t Address, uint64_t Size,
                                         unsigned Permissions) const;
  /// Access the same RAM bytes without changing CPU permissions.
  /// Implementations validate the complete span before effects and reject
  /// running/faulted CPUs. Unexpected engine failures must prevent further
  /// execution or device access.
  virtual llvm::Error readBacking(uint64_t Address,
                                  llvm::MutableArrayRef<uint8_t> Bytes);
  virtual llvm::Error writeBacking(uint64_t Address,
                                   llvm::ArrayRef<uint8_t> Bytes);
  /// Diagnostic snapshot of existing RAM, including after a terminal fault.
  /// Requires a stopped CPU without an active device callback. Validate the
  /// complete span before copying; never invoke MMIO, change permissions or
  /// clear fault state. This observation does not authorize guest/device
  /// access.
  virtual llvm::Error snapshotBacking(uint64_t Address,
                                      llvm::MutableArrayRef<uint8_t> Bytes);
  llvm::Expected<uint64_t> readInteger(uint64_t Address, unsigned Size);
  llvm::Error writeInteger(uint64_t Address, uint64_t Value, unsigned Size);
};
} // namespace neverd::emulation
#endif
