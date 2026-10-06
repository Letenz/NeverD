//===- MMIOAtomicTransaction.h - Explicit device atomic completion --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_CORE_MMIOATOMICTRANSACTION_H
#define NEVERD_EMULATION_CORE_MMIOATOMICTRANSACTION_H
#include "MemoryProjection.h"

#include "neverd/emulation/CPU.h"

namespace neverd::emulation {
/// Owns a pure device preview and one commit attempt. The ISA owns access
/// faults and result computation; the provider owns all device effects.
class MMIOAtomicTransaction final {
public:
  static llvm::Expected<std::unique_ptr<MMIOAtomicTransaction>>
  prepare(MemoryProjection &, uint64_t Address, unsigned Size,
          MachineRunControl, bool &DeviceFailed);
  llvm::ArrayRef<uint8_t> original() const { return Prepared.Value; }
  /// Observers see the old device and CPU state. On success the ISA must
  /// publish its already computed next state without another stop check.
  llvm::Expected<bool> commit(llvm::ArrayRef<uint8_t> Result,
                              const BackendHooks &,
                              llvm::function_ref<bool()> Rejected);

private:
  MMIOAtomicTransaction(std::unique_lock<std::recursive_mutex> Lease,
                        uint64_t Address, GuestMMIOPreparedAtomic Prepared,
                        MachineRunControl Control, bool &DeviceFailed);
  std::unique_lock<std::recursive_mutex> Lease;
  uint64_t Address;
  GuestMMIOPreparedAtomic Prepared;
  MachineRunControl Control;
  bool &DeviceFailed;
  bool Attempted = false;
};
} // namespace neverd::emulation
#endif
