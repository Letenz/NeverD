//===- X64PageTables.cpp - x64 page table projection---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64PageTables.h"

#include "../../core/ExecutionDiagnostics.h"
#include "../../core/MemoryProjection.h"
#include "X64ExceptionMonitor.h"
#include "X64Machine.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"

#include <cstring>
#include <iterator>

namespace neverd::emulation {
llvm::Expected<uint64_t> buildX64PageTables(
    MemoryProjection &Memory, bool UserMode, bool ExceptionMonitor,
    llvm::ArrayRef<ExecutionWatch> NoExecutePages, uint64_t WatchEpoch,
    bool WatchWrites, std::optional<uint64_t> WriteGuard,
    X64PageTableCache *Cache) {
  // The watch epoch occupies the bits above the gateway and write variants.
  // Neither a changed overlay nor a temporary step can reuse the other root.
  const uint64_t Variant =
      (ExceptionMonitor ? x64::gateway::ProjectionVariant : 0) |
      (WatchWrites ? 2 : 0) | (WriteGuard ? 4 : 0) | (WatchEpoch << 3);
  const uint64_t PreviousRoot = Memory.projectionRoot(GuestArchitecture::X64);
  if (!Memory.needsProjection(GuestArchitecture::X64, UserMode, Variant))
    return Memory.transportPhysical(PreviousRoot);
  const uint64_t Root = PreviousRoot == x64::FirstTableRoot
                            ? x64::SecondTableRoot
                            : x64::FirstTableRoot;
  if (Cache && Cache->update(Memory, UserMode, Root, Variant, NoExecutePages,
                             WatchWrites, WriteGuard))
    return Memory.transportPhysical(Root);
  if (Cache) {
    Cache->Root = 0;
    Cache->Leaves.clear();
    Cache->Aliases.clear();
  }
  // Watch permissions leave the validated page-table layout and RAM identity
  // intact. Reuse its child tables, but still alternate roots to flush TLBs.
  const bool Reuse =
      !Memory.needsProjection(GuestArchitecture::X64, UserMode, Variant,
                              x64::gateway::ProjectionVariant);
  if (!Reuse)
    if (auto E = Memory.validateMappings(x64::canonicalRange, !UserMode))
      return E;
  // Updating backing bytes alone does not invalidate cached translations.
  // Alternate roots after each mapping transaction, forcing a CR3 transition
  // on the next entry (PCID and global pages are disabled in this profile).
  Memory.invalidateProjection();
  if (Reuse)
    std::memcpy(Memory.data() + Root, Memory.data() + PreviousRoot,
                x64::PageSize);
  else
    std::memset(Memory.data() + Root, 0, x64::PageSize);
  uint64_t Next = x64::FirstChildTable;
  auto MapPage = [&](uint64_t VA, uint64_t Entry,
                     std::optional<uint64_t> BaseEntry = std::nullopt,
                     uint64_t Physical = 0) -> llvm::Error {
    uint64_t Table = Root;
    for (unsigned Level = x64::TableLevels; Level > 1; --Level) {
      auto Index = (VA >> (x64::PageBits + (Level - 1) * x64::TableBits)) &
                   (x64::TableEntries - 1);
      auto *Slot = Memory.data() + Table + Index * x64::WordBytes;
      uint64_t Child = llvm::support::endian::read64le(Slot);
      if (!Child) {
        if (Reuse || Next == x64::TableReserve)
          return diagnostic::error(diagnostic::PageTables);
        std::memset(Memory.data() + Next, 0, x64::PageSize);
        Child = Memory.transportPhysical(Next) | x64::Present | x64::Writable |
                x64::UserPage;
        Next += x64::PageSize;
        llvm::support::endian::write64le(Slot, Child);
      }
      Table = Memory.transportOffset(Child & x64::AddressMask);
    }
    auto Index = (VA >> x64::PageBits) & (x64::TableEntries - 1);
    const uint64_t Offset = Table + Index * x64::WordBytes;
    llvm::support::endian::write64le(Memory.data() + Offset, Entry);
    if (Cache && BaseEntry) {
      Cache->Aliases.emplace(Physical, Cache->Leaves.size());
      Cache->Leaves.push_back({VA, Offset, Physical, *BaseEntry});
    }
    return llvm::Error::success();
  };
  const auto PhysicalWrites = Memory.physicalWriteWatches();
  for (const auto &[VA, P] : Memory.mappings()) {
    if (P.IO) {
      // One bounded operand may use private scratch to obtain the original
      // instruction's exact flags/registers. No device backing enters the VM.
      const auto Operand = Memory.deviceOperand();
      if (!UserMode && Operand && Operand->first == VA)
        if (auto E =
                MapPage(VA, Memory.transportPhysical(Operand->second) |
                                x64::Present | x64::Writable | x64::NoExecute))
          return E;
      continue;
    }
    uint64_t Entry = Memory.transportPhysical(P.Physical);
    if (P.Permissions & GuestAccessPermissions)
      Entry |= x64::Present;
    if (UserMode && (P.Permissions & UserAccessible))
      Entry |= x64::UserPage;
    if (P.Permissions & Write)
      Entry |= x64::Writable;
    // A watched page is non-executable whatever its permissions, so a direct
    // run's first fetch into it faults at the watch boundary.
    if (!(P.Permissions & Execute))
      Entry |= x64::NoExecute;
    const uint64_t BaseEntry = Entry;
    Entry = x64WatchEntry(Entry, VA, P.Physical, NoExecutePages, PhysicalWrites,
                          WatchWrites, WriteGuard);
    if (auto E = MapPage(VA, Entry, BaseEntry, P.Physical))
      return E;
  }
  if (ExceptionMonitor) {
    auto Base = initializeX64ExceptionMonitor(Memory);
    if (!Base)
      return Base.takeError();
    // Monitor pages have no user bit. Read/execute code is separate from the
    // writable non-executable descriptor/IST pages, and no guest VA is hidden.
    for (uint64_t Page = 0; Page < x64::gateway::Pages; ++Page) {
      const uint64_t Physical = x64::gateway::DataGPA + Page * x64::PageSize;
      const uint64_t Rights =
          Physical == x64::gateway::CodeGPA
              ? x64::Present
              : x64::Present | x64::Writable | x64::NoExecute;
      if (auto E = MapPage(*Base + Page * x64::PageSize,
                           Memory.transportPhysical(Physical) | Rights))
        return E;
    }
  }
  Memory.commitProjection(GuestArchitecture::X64, UserMode, Variant, Root);
  if (Cache)
    Cache->remember(Memory, Root, Variant, NoExecutePages, WatchWrites,
                    WriteGuard);
  return Memory.transportPhysical(Root);
}
} // namespace neverd::emulation
