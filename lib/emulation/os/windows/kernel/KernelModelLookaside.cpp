//===- KernelModelLookaside.cpp - Lookaside registration and retirement ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Default pool callbacks and caller-owned x64 GENERAL_LOOKASIDE records.
/// Cache contents and counters remain in guest bytes, including WDK inlines.
///
//===----------------------------------------------------------------------===//

#include "KernelException.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <tuple>

namespace neverd::emulation {
namespace {
using namespace windows;
llvm::Error lookasideError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
bool overlaps(uint64_t A, uint64_t Size, uint64_t B, uint64_t Length) {
  return Size && Length && A < B + Length && B < A + Size;
}
} // namespace

llvm::Error KernelModel::writableLookasideStorage(uint64_t Address,
                                                  uint32_t Size) const {
  auto Writable = Memory.canAccess(Address, Size, Read | Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return lookasideError("lookaside operation requires writable storage");
  return Memory.validateBacking(Address, Size);
}

llvm::Error KernelModel::validateLookasideAccess(uint64_t Address,
                                                 uint32_t Size,
                                                 bool IsWrite) const {
  for (uint64_t Head : LookasideHeads)
    if (Head && overlaps(Address, Size, Head, ListEntrySize))
      return lookasideError("guest access to a private lookaside registry");
  for (const auto &[Base, List] : Lookasides) {
    if (!overlaps(Address, Size, Base, LookasideSize))
      continue;
    if (List.Paged && CurrentIRQL > APCLevel)
      return lookasideError(
          "paged lookaside access requires IRQL <= APC_LEVEL");
    if (!IsWrite)
      continue;
    // WDK allocation/free inlines modify the SLIST and four ULONG counters.
    // Tuning, pool callbacks and registration links belong to this model.
    const uint64_t First = std::max(Address, Base) - Base;
    const uint64_t Last = std::min(Address + Size, Base + LookasideSize) - Base;
    if (!((First < SListHeaderSize && Last <= SListHeaderSize) ||
          (First >= LookasideCountersOffset && Last <= LookasideTypeOffset)))
      return lookasideError("guest write to kernel-owned lookaside fields");
  }
  return llvm::Error::success();
}

llvm::Expected<std::vector<uint64_t>>
KernelModel::lookasideEntries(uint64_t Address) const {
  const auto &List = Lookasides.at(Address);
  auto Depth = Memory.readInteger(Address, 2);
  if (!Depth)
    return Depth.takeError();
  auto First = Memory.readInteger(Address + SListHeaderEntryOffset, 8);
  if (!First)
    return First.takeError();
  std::vector<uint64_t> Entries;
  std::set<uint64_t> Seen;
  uint64_t Entry = *First & ~SListReservedMask;
  while (Entry) {
    if ((Entry & (SListAlignment - 1)) || !Seen.insert(Entry).second)
      return lookasideError(
          "lookaside cache contains an unaligned or cyclic entry");
    auto Pool = Allocations.find(Entry);
    if (Pool == Allocations.end() || Pool->second.Size != List.EntrySize ||
        Pool->second.Tag != List.Tag || Pool->second.NonPaged == List.Paged)
      return lookasideError(
          "lookaside cache requires matching live pool entries");
    if (Entries.size() >= *Depth)
      return lookasideError("lookaside cache depth does not match its chain");
    Entries.push_back(Entry);
    auto Next = Memory.readInteger(Entry, 8);
    if (!Next)
      return Next.takeError();
    Entry = *Next;
  }
  if (Entries.size() != *Depth)
    return lookasideError("lookaside cache depth does not match its chain");
  return Entries;
}

llvm::Error KernelModel::canReleaseLookasideRange(uint64_t Address,
                                                  uint64_t Size,
                                                  uint64_t Retiring) const {
  for (uint64_t Head : LookasideHeads)
    if (Head && overlaps(Address, Size, Head, ListEntrySize))
      return lookasideError("cannot release a private lookaside registry");
  for (const auto &[Base, List] : Lookasides) {
    if (overlaps(Address, Size, Base, LookasideSize))
      return lookasideError("cannot release a live lookaside list");
    if (Base == Retiring)
      continue;
    auto Entries = lookasideEntries(Base);
    if (!Entries)
      return Entries.takeError();
    for (uint64_t Entry : *Entries)
      if (overlaps(Address, Size, Entry, List.EntrySize))
        return lookasideError(
            "cannot release an entry cached in a lookaside list");
  }
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelModel::initializeLookaside(llvm::ArrayRef<uint64_t> A, bool Paged) {
  // Export RVAs 0x37a130 and 0x6fb910 wrap the common 96-byte x64 layout.
  const uint64_t Address = A[0];
  if (Address & (SListAlignment - 1))
    return llvm::make_error<KernelGuestException>(
        exceptions::StatusDatatypeMisalignment);
  if (Address < profile::UserProbeLimit || LookasideSize > UINT64_MAX - Address)
    return lookasideError("lookaside list requires resident kernel storage");
  if (Lookasides.count(Address))
    return lookasideError("lookaside list is already initialized");
  const uint32_t Flags = uint32_t(A[3]), Size = uint32_t(A[4]),
                 Tag = uint32_t(A[5]);
  if (A[1] || A[2])
    return lookasideError(
        "custom lookaside allocation/free callbacks are unsupported");
  if ((Flags != 0 && (Paged || Flags != PoolNX)) || uint16_t(A[6]))
    return lookasideError(
        "unsupported lookaside flags or nonzero reserved depth");
  if (Size < SingleListEntrySize || Size > profile::KernelArenaSize || !Tag)
    return lookasideError("lookaside entries require a bounded size of at "
                          "least 8 and a nonzero tag");
  if (auto E = validateGuestAccess(Address, LookasideSize, true))
    return E;
  for (const auto &[Base, Allocation] : Allocations)
    if (overlaps(Address, LookasideSize, Base, Allocation.Size) &&
        !Allocation.NonPaged)
      return lookasideError("lookaside list storage must be nonpaged");
  if (auto E = writableLookasideStorage(Address, LookasideSize))
    return E;
  if (auto E = validateUnborrowedKernelStorage(Address, LookasideSize))
    return E;
  if (!Exports)
    return lookasideError(
        "lookaside callbacks require the session export registry");
  auto Allocate = Exports->resolve("ExAllocatePoolWithTag");
  if (!Allocate)
    return Allocate.takeError();
  auto Free = Exports->resolve("ExFreePool");
  if (!Free)
    return Free.takeError();
  if (!*Allocate || !*Free)
    return lookasideError("default lookaside pool callbacks are unavailable");

  uint64_t Head = LookasideHeads[Paged];
  uint64_t Tail = Head;
  if (Head) {
    if (auto E = writableLookasideStorage(Head, ListEntrySize))
      return E;
    auto Stored = Memory.readInteger(Head + ListEntryBlinkOffset, 8);
    if (!Stored)
      return Stored.takeError();
    Tail = *Stored;
    if (Tail != Head) {
      auto Prior = Lookasides.find(Tail - LookasideLinksOffset);
      if (Prior == Lookasides.end() || Prior->second.Paged != Paged)
        return lookasideError("corrupted lookaside registration tail");
    }
    if (auto E = writableLookasideStorage(Tail, ListEntrySize))
      return E;
    auto Next = Memory.readInteger(Tail, 8);
    if (!Next)
      return Next.takeError();
    if (*Next != Head)
      return lookasideError("corrupted lookaside registration links");
  } else {
    auto Storage = allocate(ListEntrySize, SListAlignment);
    if (!Storage)
      return Storage.takeError();
    Head = Tail = *Storage;
    if (auto E = Memory.writeInteger(Head, Head, 8))
      return E;
    if (auto E = Memory.writeInteger(Head + ListEntryBlinkOffset, Head, 8))
      return E;
    LookasideHeads[Paged] = Head;
  }
  const uint32_t Type = Flags | (Paged ? PoolPaged : 0);
  std::vector<uint8_t> Bytes(LookasideFutureOffset);
  llvm::support::endian::write16le(Bytes.data() + LookasideDepthOffset,
                                   profile::LookasideInitialDepth);
  llvm::support::endian::write16le(Bytes.data() + LookasideMaximumDepthOffset,
                                   profile::LookasideMaximumDepth);
  llvm::support::endian::write32le(Bytes.data() + LookasideTypeOffset, Type);
  llvm::support::endian::write32le(Bytes.data() + LookasideTagOffset, Tag);
  llvm::support::endian::write32le(Bytes.data() + LookasideEntrySizeOffset,
                                   Size);
  llvm::support::endian::write64le(Bytes.data() + LookasideAllocateOffset,
                                   *Allocate);
  llvm::support::endian::write64le(Bytes.data() + LookasideFreeOffset, *Free);
  llvm::support::endian::write64le(Bytes.data() + LookasideLinksOffset, Head);
  llvm::support::endian::write64le(
      Bytes.data() + LookasideLinksOffset + ListEntryBlinkOffset, Tail);
  if (auto E = Memory.write(Address, Bytes))
    return E;
  if (auto E = Memory.writeInteger(Tail, Address + LookasideLinksOffset, 8))
    return E;
  if (auto E = Memory.writeInteger(Head + ListEntryBlinkOffset,
                                   Address + LookasideLinksOffset, 8))
    return E;
  Lookasides.emplace(Address,
                     Lookaside{Paged, Type, Size, Tag, *Allocate, *Free});
  return 0;
}

llvm::Expected<uint64_t> KernelModel::deleteLookaside(uint64_t Address,
                                                      bool Paged) {
  // Export RVAs 0x39a420 and 0x317760 remove the registration, set Depth=0,
  // flush the SLIST, then invoke Free(entry) for every detached cache entry.
  auto It = Lookasides.find(Address);
  if (It == Lookasides.end() || It->second.Paged != Paged)
    return lookasideError(
        "lookaside delete requires a live list of the matching kind");
  if (auto E = validateGuestAccess(Address, LookasideSize, false))
    return E;
  if (auto E = writableLookasideStorage(Address, LookasideSize))
    return E;
  const Lookaside &List = It->second;
  for (auto [Offset, Value, Width] :
       {std::tuple<uint32_t, uint64_t, unsigned>{
            LookasideDepthOffset, profile::LookasideInitialDepth, 2},
        {LookasideMaximumDepthOffset, profile::LookasideMaximumDepth, 2},
        {LookasideTypeOffset, List.Type, 4},
        {LookasideTagOffset, List.Tag, 4},
        {LookasideEntrySizeOffset, List.EntrySize, 4},
        {LookasideAllocateOffset, List.Allocate, 8},
        {LookasideFreeOffset, List.Free, 8}}) {
    auto Stored = Memory.readInteger(Address + Offset, Width);
    if (!Stored)
      return Stored.takeError();
    if (*Stored != Value)
      return lookasideError("kernel-owned lookaside metadata was modified");
  }
  auto Forward = Memory.readInteger(Address + LookasideLinksOffset, 8);
  if (!Forward)
    return Forward.takeError();
  auto Backward = Memory.readInteger(
      Address + LookasideLinksOffset + ListEntryBlinkOffset, 8);
  if (!Backward)
    return Backward.takeError();
  for (auto [Link, Offset] : {std::pair{*Forward, ListEntryBlinkOffset},
                              {*Backward, ListEntryFlinkOffset}}) {
    if (Link == Address + LookasideLinksOffset)
      return lookasideError("corrupted lookaside registration self-link");
    if (Link != LookasideHeads[Paged]) {
      auto Neighbor = Lookasides.find(Link - LookasideLinksOffset);
      if (Neighbor == Lookasides.end() || Neighbor->second.Paged != Paged)
        return lookasideError("corrupted lookaside registration links");
    }
    if (auto E = writableLookasideStorage(Link, ListEntrySize))
      return E;
    auto Stored = Memory.readInteger(Link + Offset, 8);
    if (!Stored)
      return Stored.takeError();
    if (*Stored != Address + LookasideLinksOffset)
      return lookasideError("corrupted lookaside registration links; kernel "
                            "FAST_FAIL_CORRUPT_LIST_ENTRY");
  }
  auto Entries = lookasideEntries(Address);
  if (!Entries)
    return Entries.takeError();
  // Preflight every free and both neighbors before removing any ownership.
  // The retiring cache is the only excluded reference; another list, DMA pin,
  // waiter, held lock or immutable backing must still prevent retirement.
  for (uint64_t Entry : *Entries) {
    if (auto E = Physical.canRetire(Entry))
      return E;
    if (auto E = canReleaseRange(Entry, List.EntrySize, 0, Address))
      return E;
    if (auto E = writableLookasideStorage(Entry, List.EntrySize))
      return E;
  }
  auto Flushed = flushSList(Address);
  if (!Flushed)
    return Flushed.takeError();
  if (auto E = Memory.writeInteger(*Backward, *Forward, 8))
    return E;
  if (auto E =
          Memory.writeInteger(*Forward + ListEntryBlinkOffset, *Backward, 8))
    return E;
  if (auto E = Memory.writeInteger(Address + LookasideDepthOffset, 0, 2))
    return E;
  if (auto E = Memory.writeInteger(Address + LookasideLinksOffset,
                                   profile::LookasideRetiredLink, 8))
    return E;
  Lookasides.erase(It);
  for (uint64_t Entry : *Entries) {
    auto Freed = freePool({Entry}, false);
    if (!Freed)
      return Freed.takeError();
  }
  return 0;
}
} // namespace neverd::emulation
