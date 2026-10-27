//===- KernelModelExecutiveResources.cpp - ERESOURCE ownership -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Legacy executive resources share logical-thread and APC state. Guest
/// records and owner tables are opaque serializations of this single owner.
/// Contended waits and priority boosting require unsupported kernel scheduling.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace windows;
llvm::Error resourceError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
bool intersects(uint64_t A, uint64_t Size, uint64_t B, uint64_t Length) {
  return Size && Length && A < B + Length && B < A + Size;
}
} // namespace

bool KernelModel::threadHoldsExecutiveResource(uint64_t Thread) const {
  for (const auto &[Address, State] : ExecutiveResources)
    for (const auto &Owner : State.Owners)
      if (Owner.Count && Owner.Thread == Thread)
        return true;
  return false;
}

llvm::Error KernelModel::validateExecutiveResourceAccess(uint64_t Address,
                                                         uint64_t Size) const {
  if (ExecutiveResourceHead &&
      intersects(Address, Size, ExecutiveResourceHead, ListEntrySize))
    return resourceError(
        "guest access to an opaque executive resource registry");
  for (const auto &[Base, State] : ExecutiveResources) {
    if (intersects(Address, Size, Base, ExecutiveResourceSize) ||
        (State.Table &&
         intersects(Address, Size, State.Table,
                    State.Owners.size() * ExecutiveResourceOwnerSize)))
      return resourceError("guest access to an opaque executive resource");
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::canReleaseExecutiveResourceRange(uint64_t Address,
                                                          uint64_t Size) const {
  if (auto E = validateExecutiveResourceAccess(Address, Size))
    return E;
  for (const auto &[Base, State] : ExecutiveResources)
    for (const auto &Owner : State.Owners)
      if (Owner.Count && !Owner.Thread &&
          intersects(Address, Size, Owner.Value & ~uint64_t(3), 1))
        return resourceError(
            "cannot retire an executive resource owner pointer");
  return llvm::Error::success();
}

llvm::Error
KernelModel::validateExecutiveResourceMemory(uint64_t Address, uint32_t Size,
                                             bool WriteAccess) const {
  auto Allowed =
      Memory.canAccess(Address, Size, WriteAccess ? Read | Write : Read);
  if (!Allowed)
    return Allowed.takeError();
  if (!*Allowed)
    return resourceError("executive resource storage is not readable/writable");
  return Memory.validateBacking(Address, Size);
}

std::vector<uint8_t>
KernelModel::executiveResourceBytes(const ExecutiveResource &State,
                                    bool Table) const {
  using namespace llvm::support::endian;
  std::vector<uint8_t> Bytes(Table ? State.Owners.size() *
                                         ExecutiveResourceOwnerSize
                                   : ExecutiveResourceSize);
  auto WriteOwner = [&](uint64_t Offset, const ExecutiveResourceOwner &Owner) {
    write64le(Bytes.data() + Offset, Owner.Value);
    write32le(Bytes.data() + Offset + 8, Owner.Count << 3);
  };
  if (Table) {
    write32le(Bytes.data() + 8, State.Owners.size());
    for (size_t I = 1; I < State.Owners.size(); ++I)
      WriteOwner(I * ExecutiveResourceOwnerSize, State.Owners[I]);
    return Bytes;
  }
  const uint32_t Active =
      std::count_if(State.Owners.begin(), State.Owners.end(),
                    [](const auto &Owner) { return Owner.Count != 0; });
  write64le(Bytes.data(), State.Flink);
  write64le(Bytes.data() + ListEntryBlinkOffset, State.Blink);
  write64le(Bytes.data() + ExecutiveResourceTableOffset, State.Table);
  write16le(Bytes.data() + ExecutiveResourceActiveOffset, Active ? 1 : 0);
  write16le(Bytes.data() + ExecutiveResourceFlagsOffset,
            (State.Exclusive ? ExecutiveResourceExclusiveFlag : 0) |
                (State.BoostDisabled ? ExecutiveResourceDisableBoostFlag : 0));
  WriteOwner(ExecutiveResourceOwnerOffset, State.Owners.front());
  write32le(Bytes.data() + ExecutiveResourceActiveEntriesOffset, Active);
  return Bytes;
}

llvm::Expected<KernelModel::ExecutiveResource *>
KernelModel::executiveResource(uint64_t Address, bool WriteAccess,
                               bool AllowTransferred) {
  auto It = ExecutiveResources.find(Address);
  if (It == ExecutiveResources.end())
    return resourceError("executive resource is not initialized");
  auto &State = It->second;
  if (!AllowTransferred &&
      std::any_of(State.Owners.begin(), State.Owners.end(),
                  [](const auto &O) { return O.Count && !O.Thread; }))
    return resourceError("transferred executive resource permits only release "
                         "for its owner pointer");
  auto Check = [&](uint64_t Base, bool Table) -> llvm::Error {
    const auto Expected = executiveResourceBytes(State, Table);
    if (auto E =
            validateExecutiveResourceMemory(Base, Expected.size(), WriteAccess))
      return E;
    std::vector<uint8_t> Bytes(Expected.size());
    if (auto E = Memory.read(Base, Bytes))
      return E;
    if (Bytes != Expected)
      return resourceError("opaque executive resource bytes were modified");
    return llvm::Error::success();
  };
  if (auto E = Check(Address, false))
    return E;
  if (State.Table)
    if (auto E = Check(State.Table, true))
      return E;
  return &State;
}

llvm::Error
KernelModel::publishExecutiveResource(uint64_t Address,
                                      const ExecutiveResource &State) {
  // The caller preflights all existing storage before changing any state.
  if (auto E = Memory.write(Address, executiveResourceBytes(State, false)))
    return E;
  if (State.Table)
    if (auto E = Memory.write(State.Table, executiveResourceBytes(State, true)))
      return E;
  ExecutiveResources[Address] = State;
  return llvm::Error::success();
}

llvm::Error KernelModel::validateExecutiveResourceRegistry() const {
  if (!ExecutiveResourceHead)
    return llvm::Error::success();
  if (auto E = validateExecutiveResourceMemory(ExecutiveResourceHead,
                                               ListEntrySize, true))
    return E;
  uint64_t First = ExecutiveResourceHead, Last = ExecutiveResourceHead;
  for (const auto &[Base, State] : ExecutiveResources) {
    if (State.Blink == ExecutiveResourceHead)
      First = Base;
    if (State.Flink == ExecutiveResourceHead)
      Last = Base;
  }
  auto Flink = Memory.readInteger(ExecutiveResourceHead, 8);
  if (!Flink)
    return Flink.takeError();
  auto Blink =
      Memory.readInteger(ExecutiveResourceHead + ListEntryBlinkOffset, 8);
  if (!Blink)
    return Blink.takeError();
  if (*Flink != First || *Blink != Last)
    return resourceError("executive resource registry links were modified");
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelModel::initializeExecutiveResource(uint64_t Address) {
  if (Address < profile::UserProbeLimit || (Address & 7) ||
      ExecutiveResourceSize > UINT64_MAX - Address)
    return resourceError(
        "executive resource requires aligned resident kernel storage");
  if (ExecutiveResources.contains(Address))
    return resourceError("executive resource is already initialized");
  if (auto E = validateGuestAccess(Address, ExecutiveResourceSize, true))
    return E;
  for (const auto &[Base, Allocation] : Allocations)
    if (!Allocation.NonPaged &&
        intersects(Address, ExecutiveResourceSize, Base, Allocation.Size))
      return resourceError("executive resource storage must be nonpaged");
  if (auto E = validateUnborrowedKernelStorage(Address, ExecutiveResourceSize))
    return E;
  if (auto E = canRevokeVirtualRange(Address, ExecutiveResourceSize))
    return E;
  if (auto E =
          validateExecutiveResourceMemory(Address, ExecutiveResourceSize, true))
    return E;
  if (auto E = validateExecutiveResourceRegistry())
    return E;
  uint64_t Tail = ExecutiveResourceHead;
  if (Tail) {
    auto Value = Memory.readInteger(Tail + ListEntryBlinkOffset, 8);
    if (!Value)
      return Value.takeError();
    Tail = *Value;
    if (Tail != ExecutiveResourceHead) {
      auto Previous = executiveResource(Tail, true, true);
      if (!Previous)
        return Previous.takeError();
    }
  } else {
    auto Head = allocate(ListEntrySize);
    if (!Head)
      return Head.takeError();
    ExecutiveResourceHead = Tail = *Head;
  }
  ExecutiveResource State;
  State.Flink = ExecutiveResourceHead;
  State.Blink = Tail;
  if (auto E = publishExecutiveResource(Address, State))
    return E;
  if (auto E = Memory.writeInteger(Tail, Address, 8))
    return E;
  if (Tail != ExecutiveResourceHead)
    ExecutiveResources.at(Tail).Flink = Address;
  if (auto E = Memory.writeInteger(ExecutiveResourceHead + ListEntryBlinkOffset,
                                   Address, 8))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t> KernelModel::resetExecutiveResource(uint64_t Address,
                                                             bool Delete) {
  auto Found = executiveResource(Address, true);
  if (!Found)
    return Found.takeError();
  ExecutiveResource State = **Found;
  if (std::any_of(State.Owners.begin(), State.Owners.end(),
                  [](const auto &O) { return O.Count != 0; }))
    return resourceError(
        "cannot reinitialize or delete an owned executive resource");
  if (!Delete) {
    State.Exclusive = State.BoostDisabled = false;
    if (auto E = publishExecutiveResource(Address, State))
      return E;
    return StatusSuccess;
  }
  if (auto E = validateExecutiveResourceRegistry())
    return E;
  for (uint64_t Neighbor : {State.Flink, State.Blink}) {
    if (Neighbor == ExecutiveResourceHead)
      continue;
    auto Other = executiveResource(Neighbor, true, true);
    if (!Other)
      return Other.takeError();
    if ((Neighbor == State.Flink && (**Other).Blink != Address) ||
        (Neighbor == State.Blink && (**Other).Flink != Address))
      return resourceError(
          "executive resource registration links are inconsistent");
  }
  if (State.Table)
    if (auto E = Physical.canReleaseRange(
            State.Table, State.Owners.size() * ExecutiveResourceOwnerSize))
      return E;
  if (auto E = Memory.writeInteger(State.Blink, State.Flink, 8))
    return E;
  if (auto E = Memory.writeInteger(State.Flink + ListEntryBlinkOffset,
                                   State.Blink, 8))
    return E;
  if (State.Blink != ExecutiveResourceHead)
    ExecutiveResources.at(State.Blink).Flink = State.Flink;
  if (State.Flink != ExecutiveResourceHead)
    ExecutiveResources.at(State.Flink).Blink = State.Blink;
  // ExDeleteResourceLite clears only the list links, retaining other bytes,
  // including the pointer to its retired owner table.
  if (auto E = Memory.writeInteger(Address, 0, 8))
    return E;
  if (auto E = Memory.writeInteger(Address + ListEntryBlinkOffset, 0, 8))
    return E;
  if (State.Table)
    FreedRanges.emplace(State.Table,
                        State.Owners.size() * ExecutiveResourceOwnerSize);
  ExecutiveResources.erase(Address);
  return StatusSuccess;
}

llvm::Error KernelModel::validateExecutiveResourceContext(
    uint8_t IRQL, uint16_t Critical, uint16_t Guarded, uint64_t Retiring,
    uint64_t Owner, bool IgnoreMutex) const {
  if (IRQL >= APCLevel || Critical || Guarded ||
      (!IgnoreMutex && Dispatcher.ownsMutex(CurrentThreadKey)))
    return llvm::Error::success();
  for (const auto &[Address, State] : ExecutiveResources)
    for (const auto &Held : State.Owners)
      if (Held.Count && Held.Thread == CurrentThreadKey &&
          !(Address == Retiring && Held.Value == Owner && Held.Count == 1))
        return resourceError(
            "held executive resource requires normal kernel APCs disabled");
  return llvm::Error::success();
}

llvm::Error
KernelModel::validateExecutiveResourceRestoreIRQL(uint8_t IRQL) const {
  const auto State = ApcStates.find(CurrentThreadKey);
  return validateExecutiveResourceContext(
      IRQL, State == ApcStates.end() ? 0 : State->second.CriticalDepth,
      State == ApcStates.end() ? 0 : State->second.GuardedDepth);
}

llvm::Expected<uint64_t>
KernelModel::acquireExecutiveResource(uint64_t Address,
                                      ExecutiveResourceMode Mode, bool Wait,
                                      bool EnterCritical) {
  const bool Exclusive = Mode == ExecutiveResourceMode::Exclusive;
  auto Apc = apcStateForCall();
  if (!Apc)
    return Apc.takeError();
  if (EnterCritical && (*Apc)->CriticalDepth == profile::MaxAPCRegionNesting)
    return resourceError("critical-region nesting limit exceeded");
  if (!EnterCritical && CurrentIRQL < APCLevel && !(*Apc)->CriticalDepth &&
      !(*Apc)->GuardedDepth && !Dispatcher.ownsMutex(CurrentThreadKey))
    return resourceError(
        "executive resource acquisition requires normal kernel APCs disabled");
  auto Found = executiveResource(Address, true);
  if (!Found)
    return Found.takeError();
  ExecutiveResource State = **Found;
  const bool Active = std::any_of(State.Owners.begin(), State.Owners.end(),
                                  [](const auto &O) { return O.Count != 0; });
  auto Own = std::find_if(
      State.Owners.begin(), State.Owners.end(),
      [&](const auto &O) { return O.Count && O.Thread == CurrentThreadKey; });
  const bool Recursive =
      Own != State.Owners.end() && (!Exclusive || State.Exclusive);
  const bool Busy = Active && !Recursive && (Exclusive || State.Exclusive);
  if (Busy && Wait)
    return resourceError("contended executive resource acquisition requires "
                         "unsupported priority-boosted waiting");
  if (Busy &&
      (Exclusive || Mode == ExecutiveResourceMode::SharedWaitForExclusive))
    return 0;
  uint64_t ThreadObject = 0;
  if (!Busy && !Recursive) {
    if (std::count_if(State.Owners.begin(), State.Owners.end(),
                      [](const auto &O) { return O.Count != 0; }) >=
        profile::MaxConcurrentCallbacks)
      return resourceError("executive resource owner limit exceeded");
    auto Thread = currentThreadObject();
    if (!Thread)
      return Thread.takeError();
    ThreadObject = *Thread;
  }
  const uint64_t OldTable = State.Table,
                 OldTableSize =
                     State.Owners.size() * ExecutiveResourceOwnerSize;
  if (Recursive) {
    if (Own->Count == 0x1fffffff)
      return resourceError("executive resource recursion count overflow");
    ++Own->Count;
  } else {
    size_t Slot = 0;
    if (Active) {
      // WaitForExclusive reserves the inline slot while other owners remain,
      // even with no exclusive waiter. Other shared forms reuse that slot.
      if (Mode == ExecutiveResourceMode::SharedWaitForExclusive)
        Slot = 1;
      // A failed shared attempt against an exclusive owner still reserves a
      // table slot. The target retains that allocation on Wait=FALSE.
      for (; Slot < State.Owners.size(); ++Slot)
        if (!State.Owners[Slot].Count)
          break;
      if (Slot == State.Owners.size()) {
        const size_t OldSize = State.Owners.size();
        const size_t NewSize = State.Table ? OldSize + 4 : 3;
        if (NewSize > profile::MaxConcurrentCallbacks + 3)
          return resourceError("executive resource owner-table limit exceeded");
        if (State.Table)
          if (auto E = Physical.canReleaseRange(
                  State.Table, OldSize * ExecutiveResourceOwnerSize))
            return E;
        auto Table = allocate(NewSize * ExecutiveResourceOwnerSize);
        if (!Table)
          return Table.takeError();
        State.Table = *Table;
        State.Owners.resize(NewSize);
      }
    }
    if (!Busy) {
      State.Owners[Slot] = {CurrentThreadKey, ThreadObject, 1};
      State.Exclusive = Exclusive;
    }
  }
  if (auto E = publishExecutiveResource(Address, State))
    return E;
  if (OldTable && OldTable != State.Table)
    FreedRanges.emplace(OldTable, OldTableSize);
  if (EnterCritical)
    ++(*Apc)->CriticalDepth;
  // These wrappers return an opaque, driver-ignored system pointer. This
  // bounded profile has no associated system structure, as in the oracle.
  return EnterCritical ? 0 : (Busy ? 0 : 1);
}

llvm::Expected<uint64_t>
KernelModel::releaseExecutiveResource(uint64_t Address, uint64_t Owner,
                                      bool LeaveCritical) {
  auto Apc = apcStateForCall();
  if (!Apc)
    return Apc.takeError();
  if (LeaveCritical && !(*Apc)->CriticalDepth)
    return resourceError("resource release has no matching critical region");
  if (CurrentIRQL < APCLevel && !(*Apc)->CriticalDepth &&
      !(*Apc)->GuardedDepth && !Dispatcher.ownsMutex(CurrentThreadKey))
    return resourceError(
        "executive resource release requires normal kernel APCs disabled");
  auto Found = executiveResource(Address, true, Owner && !LeaveCritical);
  if (!Found)
    return Found.takeError();
  ExecutiveResource State = **Found;
  auto Held = std::find_if(
      State.Owners.begin(), State.Owners.end(), [&](const auto &O) {
        return O.Count &&
               (Owner ? O.Value == Owner : O.Thread == CurrentThreadKey);
      });
  if (Held == State.Owners.end())
    return resourceError(
        "executive resource release requires a matching owner");
  if (Held->Thread && Held->Thread != CurrentThreadKey)
    return resourceError(
        "foreign resource release requires explicit owner-pointer transfer");
  if (!Held->Thread && (!Owner || LeaveCritical))
    return resourceError(
        "transferred resource requires release for its owner pointer");
  if (LeaveCritical) {
    if (auto E = validateFastMutexContext(
            CurrentIRQL, (*Apc)->CriticalDepth - 1, (*Apc)->GuardedDepth))
      return E;
    if (auto E = validateExecutiveResourceContext(
            CurrentIRQL, (*Apc)->CriticalDepth - 1, (*Apc)->GuardedDepth,
            Address, Held->Value))
      return E;
  }
  if (!--Held->Count)
    *Held = {};
  if (std::none_of(State.Owners.begin(), State.Owners.end(),
                   [](const auto &O) { return O.Count != 0; }))
    State.Exclusive = false;
  if (auto E = publishExecutiveResource(Address, State))
    return E;
  if (LeaveCritical)
    --(*Apc)->CriticalDepth;
  return 0;
}

llvm::Expected<uint64_t> KernelModel::queryExecutiveResource(uint64_t Address,
                                                             bool Exclusive,
                                                             bool Waiters) {
  auto Found = executiveResource(Address, false);
  if (!Found)
    return Found.takeError();
  if (Waiters)
    return 0; // No blocked acquisition is admitted by this profile.
  if (!CurrentExecution || !CurrentThreadKey)
    return resourceError("executive resource query requires an active thread");
  for (const auto &Owner : (**Found).Owners)
    if (Owner.Count && Owner.Thread == CurrentThreadKey)
      return Exclusive ? uint64_t((**Found).Exclusive) : Owner.Count;
  return 0;
}

llvm::Expected<uint64_t>
KernelModel::convertExecutiveResource(uint64_t Address) {
  auto Found = executiveResource(Address, true);
  if (!Found)
    return Found.takeError();
  ExecutiveResource State = **Found;
  if (!State.Exclusive || State.Owners.front().Thread != CurrentThreadKey ||
      !State.Owners.front().Count)
    return resourceError("resource conversion requires its exclusive owner");
  const auto Apc = ApcStates.find(CurrentThreadKey);
  if (auto E = validateExecutiveResourceContext(
          CurrentIRQL, Apc == ApcStates.end() ? 0 : Apc->second.CriticalDepth,
          Apc == ApcStates.end() ? 0 : Apc->second.GuardedDepth))
    return E;
  State.Exclusive = false;
  if (auto E = publishExecutiveResource(Address, State))
    return E;
  return 0;
}

llvm::Expected<uint64_t>
KernelModel::disableExecutiveResourceBoost(uint64_t Address) {
  auto Found = executiveResource(Address, true);
  if (!Found)
    return Found.takeError();
  ExecutiveResource State = **Found;
  State.BoostDisabled = true;
  if (auto E = publishExecutiveResource(Address, State))
    return E;
  return 0;
}

llvm::Expected<uint64_t>
KernelModel::transferExecutiveResource(uint64_t Address, uint64_t Owner,
                                       uint32_t Flags) {
  if (Flags)
    return resourceError(
        "thread-referenced resource owner transfer is unsupported");
  if ((Owner & 3) != 3 || (Owner & ~uint64_t(3)) < profile::UserProbeLimit)
    return resourceError(
        "resource owner pointer requires tagged kernel storage");
  auto Found = executiveResource(Address, true);
  if (!Found)
    return Found.takeError();
  ExecutiveResource State = **Found;
  auto Held = std::find_if(
      State.Owners.begin(), State.Owners.end(),
      [&](const auto &O) { return O.Count && O.Thread == CurrentThreadKey; });
  if (Held == State.Owners.end())
    return resourceError("resource transfer requires the owning thread");
  // The tagged pointer identifies a live storage object; its type and value
  // are irrelevant, and the kernel does not dereference it.
  // https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-exsetresourceownerpointerex
  const uint64_t Storage = Owner & ~uint64_t(3);
  if (auto E = validateGuestAccess(Storage, 1, false))
    return E;
  if (auto E = Memory.validateBacking(Storage, 1))
    return E;
  auto Readable = Memory.canAccess(Storage, 1, Read);
  if (!Readable)
    return Readable.takeError();
  if (!*Readable)
    return resourceError("resource owner pointer storage is unreadable");
  Held->Thread = 0;
  Held->Value = Owner;
  if (auto E = publishExecutiveResource(Address, State))
    return E;
  return 0;
}
} // namespace neverd::emulation
