//===- X64PageTables.cpp - x64 page table projection---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
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
    llvm::ArrayRef<ExecutionWatch> NoExecutePages, uint64_t WatchEpoch) {
  // The watch epoch occupies the bits above the gateway variant, so a change
  // to the non-executable overlay rebuilds the projection and never reuses it
  // for a build without the overlay.
  const uint64_t Variant =
      (ExceptionMonitor ? x64::gateway::ProjectionVariant : 0) |
      (WatchEpoch << 1);
  // Watches are sorted and merged by the session. A watch can start inside a
  // page: cover any page intersecting the last range starting before its end.
  const auto watched = [&](uint64_t VA) {
    const auto I = llvm::upper_bound(
        NoExecutePages, VA | (x64::PageSize - 1),
        [](uint64_t VA, const ExecutionWatch &W) { return VA < W.Address; });
    return I != NoExecutePages.begin() &&
           (VA < std::prev(I)->Address ||
            VA - std::prev(I)->Address < std::prev(I)->Size);
  };
  const uint64_t PreviousRoot = Memory.projectionRoot(GuestArchitecture::X64);
  if (!Memory.needsProjection(GuestArchitecture::X64, UserMode, Variant))
    return Memory.transportPhysical(PreviousRoot);
  if (auto E = Memory.validateMappings(x64::canonicalRange, !UserMode))
    return E;
  // Updating backing bytes alone does not invalidate cached translations.
  // Alternate roots after each mapping transaction, forcing a CR3 transition
  // on the next entry (PCID and global pages are disabled in this profile).
  const uint64_t Root = PreviousRoot == x64::FirstTableRoot
                            ? x64::SecondTableRoot
                            : x64::FirstTableRoot;
  Memory.invalidateProjection();
  std::memset(Memory.data(), 0, x64::TableReserve);
  uint64_t Next = x64::FirstChildTable;
  auto MapPage = [&](uint64_t VA, uint64_t Entry) -> llvm::Error {
    uint64_t Table = Root;
    for (unsigned Level = x64::TableLevels; Level > 1; --Level) {
      auto Index = (VA >> (x64::PageBits + (Level - 1) * x64::TableBits)) &
                   (x64::TableEntries - 1);
      auto *Slot = Memory.data() + Table + Index * x64::WordBytes;
      uint64_t Child = llvm::support::endian::read64le(Slot);
      if (!Child) {
        if (Next == x64::TableReserve)
          return diagnostic::error(diagnostic::PageTables);
        Child = Memory.transportPhysical(Next) | x64::Present | x64::Writable |
                x64::UserPage;
        Next += x64::PageSize;
        llvm::support::endian::write64le(Slot, Child);
      }
      Table = Memory.transportOffset(Child & x64::AddressMask);
    }
    auto Index = (VA >> x64::PageBits) & (x64::TableEntries - 1);
    llvm::support::endian::write64le(
        Memory.data() + Table + Index * x64::WordBytes, Entry);
    return llvm::Error::success();
  };
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
    if (!(P.Permissions & Execute) || watched(VA))
      Entry |= x64::NoExecute;
    if (auto E = MapPage(VA, Entry))
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
  return Memory.transportPhysical(Root);
}
} // namespace neverd::emulation
