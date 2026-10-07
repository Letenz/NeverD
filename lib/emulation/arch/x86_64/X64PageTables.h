//===- X64PageTables.h - Private x64 watch projection cache --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ARCH_X64PAGETABLES_H
#define NEVERD_EMULATION_ARCH_X64PAGETABLES_H

#include "neverd/emulation/CPU.h"

#include <map>
#include <optional>
#include <vector>

namespace neverd::emulation {
class MemoryProjection;
uint64_t x64WatchEntry(uint64_t Base, uint64_t Address, uint64_t Physical,
                       llvm::ArrayRef<ExecutionWatch> Execution,
                       llvm::ArrayRef<MemoryWriteWatch> Writes,
                       bool ProtectWrites, std::optional<uint64_t> Guard);
/// ISA-owned locations in a validated projection. Mapping, privilege,
/// transport-window and monitor changes invalidate this cache. Watch-only
/// changes update affected leaves and alternate CR3 without rebuilding RAM.
struct X64PageTableCache {
  struct Leaf {
    uint64_t Address, Offset, Physical, BaseEntry;
  };
  std::vector<Leaf> Leaves;
  std::multimap<uint64_t, size_t> Aliases;
  uint64_t Root = 0, Variant = 0;
  bool WatchWrites = false;
  std::optional<uint64_t> WriteGuard;
  std::vector<ExecutionWatch> Execution;
  std::vector<MemoryWriteWatch> Writes;

  bool update(MemoryProjection &Memory, bool UserMode, uint64_t NextRoot,
              uint64_t NextVariant, llvm::ArrayRef<ExecutionWatch> Watches,
              bool ProtectWrites, std::optional<uint64_t> Guard);
  void remember(MemoryProjection &Memory, uint64_t NextRoot,
                uint64_t NextVariant, llvm::ArrayRef<ExecutionWatch> Watches,
                bool ProtectWrites, std::optional<uint64_t> Guard);
};
} // namespace neverd::emulation
#endif
