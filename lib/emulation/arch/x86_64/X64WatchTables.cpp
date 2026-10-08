//===- X64WatchTables.cpp - Update private watch translation permissions --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../core/MemoryProjection.h"
#include "X64ExceptionMonitor.h"
#include "X64Machine.h"
#include "X64PageTables.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"

#include <cstring>
#include <iterator>

namespace neverd::emulation {
namespace {
template <typename Range>
bool watched(llvm::ArrayRef<Range> Ranges, uint64_t Page) {
  const auto I = llvm::upper_bound(
      Ranges, Page | (x64::PageSize - 1),
      [](uint64_t Address, const Range &W) { return Address < W.Address; });
  return I != Ranges.begin() &&
         (Page < std::prev(I)->Address ||
          Page - std::prev(I)->Address < std::prev(I)->Size);
}
} // namespace

uint64_t x64WatchEntry(uint64_t Base, uint64_t Address, uint64_t Physical,
                       llvm::ArrayRef<ExecutionWatch> Execution,
                       llvm::ArrayRef<MemoryWriteWatch> Writes,
                       bool ProtectWrites, std::optional<uint64_t> Guard) {
  if (watched(Execution, Address))
    Base |= x64::NoExecute;
  if ((ProtectWrites && watched(Writes, Physical)) || Guard == Physical)
    Base &= ~x64::Writable;
  return Base;
}

bool X64PageTableCache::update(MemoryProjection &Memory, bool UserMode,
                               uint64_t NextRoot, uint64_t NextVariant,
                               llvm::ArrayRef<ExecutionWatch> Watches,
                               bool ProtectWrites,
                               std::optional<uint64_t> Guard) {
  if (!Root || Root != Memory.projectionRoot(GuestArchitecture::X64) ||
      Memory.needsProjection(GuestArchitecture::X64, UserMode, Variant) ||
      ((NextVariant ^ Variant) & x64::gateway::ProjectionVariant))
    return false;
  const auto PhysicalWrites = Memory.physicalWriteWatches();
  Memory.invalidateProjection();
  std::memcpy(Memory.data() + NextRoot, Memory.data() + Root, x64::PageSize);
  auto Update = [&](const Leaf &L) {
    const uint64_t Entry =
        x64WatchEntry(L.BaseEntry, L.Address, L.Physical, Watches,
                      PhysicalWrites, ProtectWrites, Guard);
    llvm::support::endian::write64le(Memory.data() + L.Offset, Entry);
  };
  if (!llvm::equal(Execution, Watches, [](const auto &A, const auto &B) {
        return A.Address == B.Address && A.Size == B.Size;
      })) {
    // Evaluate only intervals whose page-level execute permission changed.
    // Byte watches can overlap the same page, so compare their union instead
    // of XORing individual interval endpoints.
    std::vector<uint64_t> Boundaries;
    auto Add = [&](llvm::ArrayRef<ExecutionWatch> Ranges) {
      for (const auto &W : Ranges) {
        Boundaries.push_back(W.Address & ~(x64::PageSize - 1));
        const uint64_t Last = (W.Address + W.Size - 1) | (x64::PageSize - 1);
        if (Last != UINT64_MAX)
          Boundaries.push_back(Last + 1);
      }
    };
    Add(Execution);
    Add(Watches);
    llvm::sort(Boundaries);
    Boundaries.erase(std::unique(Boundaries.begin(), Boundaries.end()),
                     Boundaries.end());
    for (size_t I = 0; I < Boundaries.size(); ++I) {
      const uint64_t Begin = Boundaries[I];
      if (watched(llvm::ArrayRef(Execution), Begin) == watched(Watches, Begin))
        continue;
      auto L = llvm::lower_bound(Leaves, Begin, [](const Leaf &L, uint64_t VA) {
        return L.Address < VA;
      });
      for (; L != Leaves.end() &&
             (I + 1 == Boundaries.size() || L->Address < Boundaries[I + 1]);
           ++L)
        if (!(L->BaseEntry & x64::NoExecute))
          Update(*L);
    }
  }
  if (WatchWrites != ProtectWrites ||
      (ProtectWrites && llvm::ArrayRef(Writes) != PhysicalWrites)) {
    for (const auto &L : Leaves)
      Update(L);
  } else if (WriteGuard != Guard) {
    for (const auto &Page : {WriteGuard, Guard}) {
      if (!Page)
        continue;
      auto [Begin, End] = Aliases.equal_range(*Page);
      for (auto I = Begin; I != End; ++I)
        Update(Leaves[I->second]);
    }
  }
  Memory.commitProjection(GuestArchitecture::X64, UserMode, NextVariant,
                          NextRoot);
  remember(Memory, NextRoot, NextVariant, Watches, ProtectWrites, Guard);
  return true;
}

void X64PageTableCache::remember(MemoryProjection &Memory, uint64_t NextRoot,
                                 uint64_t NextVariant,
                                 llvm::ArrayRef<ExecutionWatch> Watches,
                                 bool ProtectWrites,
                                 std::optional<uint64_t> Guard) {
  Root = NextRoot;
  Variant = NextVariant;
  Execution.assign(Watches.begin(), Watches.end());
  WatchWrites = ProtectWrites;
  WriteGuard = Guard;
  const auto PhysicalWrites = Memory.physicalWriteWatches();
  Writes.assign(PhysicalWrites.begin(), PhysicalWrites.end());
}
} // namespace neverd::emulation
