//===- KernelModelSpinLocks.cpp - Executive spin locks --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded single-processor executive spin locks and IRQL restoration, and the
/// ExInterlocked routines that hold such a lock only for one update.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/DriverProfile.h"

namespace neverd::emulation {
namespace {
llvm::Error spinLockError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}

llvm::Error corruptListError() {
  return spinLockError("corrupted interlocked list entry; the kernel fails "
                       "fast with FAST_FAIL_CORRUPT_LIST_ENTRY");
}
} // namespace

llvm::Error KernelModel::validateSpinLockAddress(uint64_t Address) const {
  if (!Address || (Address & 7))
    return spinLockError("executive spin lock requires aligned kernel storage");
  if (Interrupts.usesSpinLock(Address))
    return spinLockError(
        "connected interrupt spin lock requires its interrupt APIs");
  return llvm::Error::success();
}

llvm::Error KernelModel::validateSpinLockStorage(uint64_t Address) const {
  if (auto E = validateGuestAccess(Address, 8, true))
    return E;
  auto Writable = Memory.canAccess(Address, 8, Read | Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return spinLockError("executive spin lock requires writable storage");
  for (const auto &[Pool, Allocation] : Allocations)
    if (Address >= Pool && Address - Pool < Allocation.Size &&
        (!Allocation.NonPaged || 8 > Allocation.Size - (Address - Pool)))
      return spinLockError("executive spin lock requires nonpaged storage");

  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelModel::initializeSpinLock(uint64_t Address) {
  if (auto E = validateSpinLockAddress(Address))
    return E;
  if (Address < profile::UserProbeLimit)
    return spinLockError("executive spin lock requires kernel storage");
  if (ExecutiveSpinLocks.count(Address))
    return spinLockError("cannot initialize a held executive spin lock");
  if (auto E = validateSpinLockStorage(Address))
    return E;
  if (auto E = Memory.writeInteger(Address, 0, 8))
    return E;
  return 0;
}

llvm::Expected<uint64_t> KernelModel::acquireSpinLock(uint64_t Address,
                                                      SpinLockMode Mode) {
  if (auto E = validateSpinLockAddress(Address))
    return E;
  if (Address < profile::UserProbeLimit)
    return spinLockError("executive spin lock requires kernel storage");
  const bool AtDpc = Mode != SpinLockMode::Raise;
  if (AtDpc && CurrentIRQL != scheduler::DispatchLevel)
    return spinLockError("DPC-level spin-lock acquisition requires "
                         "DISPATCH_LEVEL");
  if (!CurrentExecution)
    return spinLockError("executive spin lock requires an active guest thread");
  if (ExecutiveSpinLocks.count(Address)) {
    if (Mode == SpinLockMode::TryAtDpc)
      return 0;
    return spinLockError("executive spin lock would deadlock on this "
                         "cooperative processor");
  }
  if (auto E = validateSpinLockStorage(Address))
    return E;
  auto Value = Memory.readInteger(Address, 8);
  if (!Value)
    return Value.takeError();
  if (*Value)
    return spinLockError("executive spin lock must be initialized and free");
  if (auto E = Memory.writeInteger(Address, 1, 8))
    return E;
  const uint8_t OldIRQL = CurrentIRQL;
  CurrentIRQL = scheduler::DispatchLevel;
  ExecutiveSpinLocks.emplace(
      Address, ExecutiveSpinLock{CurrentExecution, OldIRQL, !AtDpc});
  if (Mode == SpinLockMode::Raise)
    return OldIRQL;
  return Mode == SpinLockMode::TryAtDpc ? 1 : 0;
}

llvm::Expected<uint64_t>
KernelModel::releaseSpinLock(uint64_t Address,
                             std::optional<uint8_t> RestoreIRQL) {
  if (auto E = validateSpinLockAddress(Address))
    return E;
  auto Lock = ExecutiveSpinLocks.find(Address);
  if (Lock == ExecutiveSpinLocks.end() ||
      Lock->second.Execution != CurrentExecution)
    return spinLockError("executive spin lock is not owned by this thread");
  if (CurrentIRQL != scheduler::DispatchLevel)
    return spinLockError("executive spin lock release requires DISPATCH_LEVEL");
  if (Lock->second.RaisedIRQL != RestoreIRQL.has_value())
    return spinLockError("executive spin lock release variant does not "
                         "match acquisition");
  if (RestoreIRQL && *RestoreIRQL != Lock->second.OldIRQL)
    return spinLockError("executive spin lock release must restore the "
                         "saved IRQL");
  auto Value = Memory.readInteger(Address, 8);
  if (!Value)
    return Value.takeError();
  if (*Value != 1)
    return spinLockError("executive spin lock storage was modified");
  if (auto E = Memory.writeInteger(Address, 0, 8))
    return E;
  CurrentIRQL = RestoreIRQL ? Lock->second.OldIRQL : scheduler::DispatchLevel;
  ExecutiveSpinLocks.erase(Lock);
  return 0;
}

// ExpAcquireSpinLockDisabled (RVA 0x2f8814) masks interrupts and spins on bit 0
// of the lock without raising IRQL; ExpReleaseSpinLockDisabled clears the lock
// and unmasks again. Each ExInterlocked routine therefore leaves the lock free
// and IRQL unchanged, so only a lock that is already held is observable: on
// this cooperative processor its holder could never run to release it.
llvm::Error KernelModel::validateInterlockedLock(uint64_t Lock) const {
  if (auto E = validateSpinLockAddress(Lock))
    return E;
  if (Lock < profile::UserProbeLimit)
    return spinLockError("executive spin lock requires kernel storage");
  if (ExecutiveSpinLocks.count(Lock))
    return spinLockError("executive spin lock would deadlock on this "
                         "cooperative processor");
  if (auto E = validateSpinLockStorage(Lock))
    return E;
  auto Value = Memory.readInteger(Lock, 8);
  if (!Value)
    return Value.takeError();
  if (*Value)
    return spinLockError("executive spin lock must be initialized and free");
  return llvm::Error::success();
}

llvm::Error KernelModel::validateInterlockedStorage(uint64_t Lock,
                                                    uint64_t Address,
                                                    uint32_t Size,
                                                    bool IsWrite) const {
  // Interrupts stay masked while the routine touches caller storage, so the
  // DDI requires it to be resident at every IRQL.
  if (Address < profile::UserProbeLimit)
    return spinLockError("interlocked storage requires resident kernel memory");
  if (auto E = validateGuestAccess(Address, Size, IsWrite))
    return E;
  if (Address < Lock + 8 && Lock < Address + Size)
    return spinLockError("interlocked storage overlaps its spin lock");
  if (IsWrite) {
    auto Writable = Memory.canAccess(Address, Size, Read | Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return spinLockError("interlocked storage requires writable memory");
  }
  for (const auto &[Pool, Allocation] : Allocations)
    if (Address < Pool + Allocation.Size && Pool < Address + Size &&
        !Allocation.NonPaged)
      return spinLockError("interlocked storage requires nonpaged memory");
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelModel::readInterlockedLink(uint64_t Lock,
                                                          uint64_t Address,
                                                          bool IsWrite) const {
  // Every link the routine later stores is validated before any store, so a
  // refused call leaves the list unchanged.
  if (auto E = validateInterlockedStorage(Lock, Address, 8, IsWrite))
    return E;
  return Memory.readInteger(Address, 8);
}

llvm::Expected<uint64_t>
KernelModel::interlockedInsertList(llvm::ArrayRef<uint64_t> A, bool Tail) {
  // ExInterlockedInsertHeadList (RVA 0x2f8650) and ExInterlockedInsertTailList
  // (RVA 0x2f86d0) fail fast unless the neighbor links back to the head, then
  // store the entry's links, the neighbor's and the head's in that order. They
  // return the old neighbor, or NULL when the list was empty.
  const uint64_t Head = A[0], Entry = A[1], Lock = A[2];
  if (auto E = validateInterlockedLock(Lock))
    return E;
  const uint64_t Near =
      Tail ? windows::ListEntryBlinkOffset : windows::ListEntryFlinkOffset;
  const uint64_t Far =
      Tail ? windows::ListEntryFlinkOffset : windows::ListEntryBlinkOffset;
  auto Neighbor = readInterlockedLink(Lock, Head + Near, true);
  if (!Neighbor)
    return Neighbor.takeError();
  auto Back = readInterlockedLink(Lock, *Neighbor + Far, true);
  if (!Back)
    return Back.takeError();
  if (*Back != Head)
    return corruptListError();
  if (auto E =
          validateInterlockedStorage(Lock, Entry, windows::ListEntrySize, true))
    return E;
  const uint64_t Flink = Tail ? Head : *Neighbor;
  const uint64_t Blink = Tail ? *Neighbor : Head;
  if (auto E =
          Memory.writeInteger(Entry + windows::ListEntryFlinkOffset, Flink, 8))
    return E;
  if (auto E =
          Memory.writeInteger(Entry + windows::ListEntryBlinkOffset, Blink, 8))
    return E;
  if (auto E = Memory.writeInteger(*Neighbor + Far, Entry, 8))
    return E;
  if (auto E = Memory.writeInteger(Head + Near, Entry, 8))
    return E;
  return *Neighbor == Head ? 0 : *Neighbor;
}

llvm::Expected<uint64_t>
KernelModel::interlockedRemoveHeadList(llvm::ArrayRef<uint64_t> A) {
  // ExInterlockedRemoveHeadList (RVA 0x2f8780) returns NULL for an empty list.
  // Otherwise it fails fast unless the first entry links back to the head and
  // the next entry back to the first, then unlinks the first entry without
  // touching its own links.
  const uint64_t Head = A[0], Lock = A[1];
  if (auto E = validateInterlockedLock(Lock))
    return E;
  auto Entry =
      readInterlockedLink(Lock, Head + windows::ListEntryFlinkOffset, true);
  if (!Entry)
    return Entry.takeError();
  if (*Entry == Head)
    return 0;
  auto Next =
      readInterlockedLink(Lock, *Entry + windows::ListEntryFlinkOffset, false);
  if (!Next)
    return Next.takeError();
  auto EntryBack =
      readInterlockedLink(Lock, *Entry + windows::ListEntryBlinkOffset, false);
  if (!EntryBack)
    return EntryBack.takeError();
  if (*EntryBack != Head)
    return corruptListError();
  auto NextBack =
      readInterlockedLink(Lock, *Next + windows::ListEntryBlinkOffset, true);
  if (!NextBack)
    return NextBack.takeError();
  if (*NextBack != *Entry)
    return corruptListError();
  if (auto E =
          Memory.writeInteger(Head + windows::ListEntryFlinkOffset, *Next, 8))
    return E;
  if (auto E =
          Memory.writeInteger(*Next + windows::ListEntryBlinkOffset, Head, 8))
    return E;
  return *Entry;
}

llvm::Expected<uint64_t>
KernelModel::interlockedPushEntryList(llvm::ArrayRef<uint64_t> A) {
  // ExInterlockedPushEntryList (RVA 0x5b6e50) returns the old first entry.
  const uint64_t Head = A[0], Entry = A[1], Lock = A[2];
  if (auto E = validateInterlockedLock(Lock))
    return E;
  auto First = readInterlockedLink(Lock, Head, true);
  if (!First)
    return First.takeError();
  if (auto E = validateInterlockedStorage(Lock, Entry,
                                          windows::SingleListEntrySize, true))
    return E;
  if (auto E = Memory.writeInteger(Entry, *First, 8))
    return E;
  if (auto E = Memory.writeInteger(Head, Entry, 8))
    return E;
  return *First;
}

llvm::Expected<uint64_t>
KernelModel::interlockedPopEntryList(llvm::ArrayRef<uint64_t> A) {
  // ExInterlockedPopEntryList (RVA 0x5b6e00) leaves the popped entry's link.
  const uint64_t Head = A[0], Lock = A[1];
  if (auto E = validateInterlockedLock(Lock))
    return E;
  auto First = readInterlockedLink(Lock, Head, true);
  if (!First)
    return First.takeError();
  if (!*First)
    return 0;
  auto Next = readInterlockedLink(Lock, *First, false);
  if (!Next)
    return Next.takeError();
  if (auto E = Memory.writeInteger(Head, *Next, 8))
    return E;
  return *First;
}

llvm::Expected<uint64_t> KernelModel::interlockedAdd(llvm::ArrayRef<uint64_t> A,
                                                     unsigned Size) {
  // ExInterlockedAddUlong (RVA 0x3795b0) and ExInterlockedAddLargeInteger
  // (RVA 0x5b6da0) store the wrapped sum and return the original value.
  const uint64_t Addend = A[0], Increment = A[1], Lock = A[2];
  if (auto E = validateInterlockedLock(Lock))
    return E;
  if (auto E = validateInterlockedStorage(Lock, Addend, Size, true))
    return E;
  auto Value = Memory.readInteger(Addend, Size);
  if (!Value)
    return Value.takeError();
  const uint64_t Mask =
      Size == 8 ? UINT64_MAX : (uint64_t(1) << (Size * 8)) - 1;
  if (auto E = Memory.writeInteger(Addend, (*Value + Increment) & Mask, Size))
    return E;
  return *Value;
}
} // namespace neverd::emulation
