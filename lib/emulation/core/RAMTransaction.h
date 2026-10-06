//===- RAMTransaction.h - Staged effects over an execution lease ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_CORE_RAMTRANSACTION_H
#define NEVERD_EMULATION_CORE_RAMTRANSACTION_H
#include "MemoryProjection.h"

namespace neverd::emulation {
struct RAMWriteRange {
  uint64_t Address, Size;
};

/// Retains only the declared physical write footprint of one instruction.
/// The ISA owns that footprint; this class never discovers instruction effects.
/// Native execution may change these bytes privately under the physical lease.
/// Staging restores original RAM before observers inspect the computed result.
class RAMTransaction final {
public:
  static llvm::Expected<std::unique_ptr<RAMTransaction>>
  create(MemoryProjection &Memory, llvm::ArrayRef<RAMWriteRange> Writes,
         uint64_t ByteBudget, unsigned Permissions = Write);
  ~RAMTransaction();
  /// Run the admitted instruction, synchronizing private transport bytes and
  /// overlapping native entries only when this transaction cannot write RAM.
  llvm::Error execute(llvm::function_ref<llvm::Error()> F,
                      llvm::ArrayRef<RAMWriteRange> Inputs);
  llvm::Error stage();
  llvm::Error read(uint64_t Address,
                   llvm::MutableArrayRef<uint8_t> Bytes) const;
  llvm::Error commit();

private:
  struct Slice {
    uint64_t Physical;
    std::vector<uint8_t> Before, After;
  };
  enum class Phase { Executing, Staged, Committed };
  RAMTransaction(MemoryProjection &Memory,
                 std::unique_lock<std::recursive_mutex> Lease,
                 std::vector<Slice> Slices);
  void restore();
  MemoryProjection &Memory;
  std::unique_lock<std::recursive_mutex> Lease;
  std::vector<Slice> Slices;
  Phase State = Phase::Executing;
};
} // namespace neverd::emulation
#endif
