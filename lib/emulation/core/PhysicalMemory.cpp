//===- PhysicalMemory.cpp - Shared RAM allocation lifetimes ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/PhysicalMemory.h"

#include "ExecutionDiagnostics.h"
#include "MemoryLayout.h"
#include "MemoryStorage.h"

#include <algorithm>
#include <cstring>

namespace neverd::emulation {
PhysicalMemory::PhysicalMemory(std::unique_ptr<Impl> State)
    : State(std::move(State)) {}
PhysicalMemory::~PhysicalMemory() {
  (void)llvm::sys::Memory::releaseMappedMemory(State->Backing);
}
llvm::Expected<std::shared_ptr<PhysicalMemory>>
PhysicalMemory::create(uint64_t Limit) {
  if (!Limit || Limit > memory::MaxRAM)
    return diagnostic::error(diagnostic::MemoryLimit);
  auto State = std::make_unique<Impl>();
  std::error_code EC;
  State->Backing = llvm::sys::Memory::allocateMappedMemory(
      Limit, nullptr, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE,
      EC);
  if (EC)
    return llvm::errorCodeToError(EC);
  State->Limit = Limit;
  State->Free.emplace(0, Limit);
  return std::shared_ptr<PhysicalMemory>(new PhysicalMemory(std::move(State)));
}
uint64_t PhysicalMemory::limit() const { return State->Limit; }
uint64_t PhysicalMemory::allocatedBytes() const { return State->Used.load(); }
llvm::Expected<std::shared_ptr<MemoryRegion>>
PhysicalMemory::allocate(uint64_t Size) {
  std::unique_lock Lock(State->Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || State->Running)
    return diagnostic::error(diagnostic::Running);
  if (!Size || Size % memory::PageSize)
    return diagnostic::error(diagnostic::InvalidMapping);
  std::lock_guard AllocationLock(State->AllocatorMutex);
  auto I =
      std::find_if(State->Free.begin(), State->Free.end(),
                   [&](const auto &Range) { return Range.second >= Size; });
  if (I == State->Free.end())
    return llvm::make_error<GuestMemoryLimitError>();
  const auto [Offset, Available] = *I;
  State->Free.erase(I);
  if (Available > Size)
    State->Free.emplace(Offset + Size, Available - Size);
  std::memset(static_cast<uint8_t *>(State->Backing.base()) + Offset, 0, Size);
  State->Used += Size;
  return std::shared_ptr<MemoryRegion>(
      new MemoryRegion(shared_from_this(), Offset, Size));
}
MemoryRegion::MemoryRegion(std::shared_ptr<PhysicalMemory> Owner,
                           uint64_t Offset, uint64_t Size)
    : Owner(std::move(Owner)), Offset(Offset), Size(Size) {}
MemoryRegion::~MemoryRegion() {
  auto &S = *Owner->State;
  std::lock_guard Lock(S.AllocatorMutex);
  uint64_t Begin = Offset, Length = Size;
  auto Next = S.Free.lower_bound(Begin);
  if (Next != S.Free.begin()) {
    auto Previous = std::prev(Next);
    if (Previous->first + Previous->second == Begin) {
      Begin = Previous->first;
      Length += Previous->second;
      S.Free.erase(Previous);
    }
  }
  if (Next != S.Free.end() && Begin + Length == Next->first) {
    Length += Next->second;
    S.Free.erase(Next);
  }
  S.Free.emplace(Begin, Length);
  S.Used -= Size;
}
} // namespace neverd::emulation
