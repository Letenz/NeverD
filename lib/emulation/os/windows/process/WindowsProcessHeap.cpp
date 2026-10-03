//===- WindowsProcessHeap.cpp - Owned Windows process heap blocks --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcess.h"

#include "neverd/emulation/CPU.h"

#include <algorithm>
#include <array>
#include <iterator>

namespace neverd::emulation::windows_process {
namespace {
using namespace value;
uint64_t mappedSize(uint64_t Size) {
  return (std::max<uint64_t>(Size, 1) + PageSize - 1) & ~(PageSize - 1);
}
} // namespace

llvm::Expected<bool> Services::mapHeapPages(uint64_t Address, uint64_t Size) {
  auto RAM = Memory.physicalMemory();
  if (Size > Options.MemoryLimit - Memory.mappedBytes() ||
      Size > RAM->limit() - RAM->allocatedBytes())
    return false;
  // Separate backing permits a shrink to return complete trailing pages.
  // Stage capacity before publishing; the guest CPU is stopped throughout.
  std::vector<std::shared_ptr<MemoryRegion>> Pages;
  Pages.reserve(Size / PageSize);
  for (uint64_t Offset = 0; Offset < Size; Offset += PageSize) {
    if (!Budget.remainingMicroseconds())
      return failure(text::HeapTimeout);
    auto Page = RAM->allocate(PageSize);
    if (!Page) {
      auto E = Page.takeError();
      if (!E.isA<GuestMemoryLimitError>())
        return std::move(E);
      llvm::consumeError(std::move(E));
      return false;
    }
    Pages.push_back(std::move(*Page));
  }
  uint64_t Published = 0;
  auto Rollback = [&](llvm::Error E) {
    if (Published)
      E = llvm::joinErrors(std::move(E), Memory.unmap(Address, Published));
    return E;
  };
  for (auto &Page : Pages) {
    if (!Budget.remainingMicroseconds())
      return Rollback(failure(text::HeapTimeout));
    if (auto E = Memory.mapRegion(Address + Published, Page, 0, PageSize,
                                  Read | Write | UserAccessible)) {
      const bool Exhausted = E.isA<GuestMemoryLimitError>();
      if (!Exhausted)
        return Rollback(std::move(E));
      llvm::consumeError(std::move(E));
      if (auto Undo = Rollback(llvm::Error::success()))
        return std::move(Undo);
      return false;
    }
    Published += PageSize;
  }
  return true;
}

llvm::Expected<uint64_t> Services::allocateHeap(uint64_t Size, bool Snapshot) {
  if (Size > Options.MemoryLimit || Size > UINT64_MAX - PageSize)
    return 0;
  const uint64_t Mapped = mappedSize(Size);
  uint64_t Address = HeapBase;
  for (const auto &[Start, Allocation] : Allocations) {
    if (!Budget.remainingMicroseconds())
      return failure(text::HeapTimeout);
    if (Mapped <= Start - Address)
      break;
    Address = Start + Allocation.MappedSize;
  }
  if (Address >= HeapLimit || Mapped > HeapLimit - Address)
    return 0;
  auto MappedPages = mapHeapPages(Address, Mapped);
  if (!MappedPages)
    return MappedPages.takeError();
  if (!*MappedPages)
    return 0;
  Allocations.emplace(Address, Allocation{Size, Mapped, Snapshot});
  // Fresh guest backing is zero-filled. Without HEAP_ZERO_MEMORY the bytes
  // are unspecified; deterministic zeroes are permitted.
  return Address;
}

llvm::Expected<uint64_t>
Services::reallocateHeap(uint64_t Address, uint64_t Size, uint32_t Flags) {
  if (Size > Options.MemoryLimit || Size > UINT64_MAX - PageSize)
    return 0;
  auto Found = Allocations.find(Address);
  auto &Old = Found->second;
  const uint64_t Mapped = mappedSize(Size);
  if (Mapped <= Old.MappedSize) {
    if (Size > Old.Size && (Flags & HeapZeroMemory)) {
      auto Writable = access(Address + Old.Size, Size - Old.Size, Write);
      if (!Writable)
        return Writable.takeError();
      if (!*Writable)
        return failure(text::UserException);
      std::vector<uint8_t> Zeroes(Size - Old.Size);
      if (auto E = CPU.write(Address + Old.Size, Zeroes))
        return std::move(E);
    }
    if (Mapped < Old.MappedSize)
      if (auto E = Memory.unmap(Address + Mapped, Old.MappedSize - Mapped))
        return std::move(E);
    Old.Size = Size;
    Old.MappedSize = Mapped;
    return Address;
  }

  const auto Next = std::next(Found);
  const uint64_t End = Next == Allocations.end() ? HeapLimit : Next->first;
  if (Mapped <= End - Address) {
    // Only old last-page padding needs clearing. New backing is already zero.
    const uint64_t Clear =
        (Flags & HeapZeroMemory) ? Old.MappedSize - Old.Size : 0;
    auto Writable = access(Address + Old.Size, Clear, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return failure(text::UserException);
    const uint64_t Extra = Mapped - Old.MappedSize;
    auto Added = mapHeapPages(Address + Old.MappedSize, Extra);
    if (!Added)
      return Added.takeError();
    if (!*Added)
      return 0;
    if (Clear) {
      std::vector<uint8_t> Zeroes(Clear);
      if (auto E = CPU.write(Address + Old.Size, Zeroes))
        return llvm::joinErrors(std::move(E),
                                Memory.unmap(Address + Old.MappedSize, Extra));
    }
    Old.Size = Size;
    Old.MappedSize = Mapped;
    return Address;
  }
  if (Flags & HeapReallocInPlaceOnly)
    return 0;

  auto Readable = access(Address, Old.Size, Read);
  if (!Readable)
    return Readable.takeError();
  if (!*Readable)
    return failure(text::UserException);
  auto Replacement = allocateHeap(Size);
  if (!Replacement)
    return Replacement.takeError();
  if (!*Replacement)
    return 0;
  auto Rollback = [&](llvm::Error E) {
    if (auto Undo = Memory.unmap(*Replacement, Mapped))
      return llvm::joinErrors(std::move(E), std::move(Undo));
    Allocations.erase(*Replacement);
    return E;
  };
  std::array<uint8_t, PageSize> Bytes;
  for (uint64_t Offset = 0; Offset < Old.Size;) {
    if (!Budget.remainingMicroseconds())
      return Rollback(failure(text::HeapTimeout));
    auto Chunk = llvm::MutableArrayRef(Bytes).take_front(
        std::min<uint64_t>(Bytes.size(), Old.Size - Offset));
    if (auto E = CPU.read(Address + Offset, Chunk))
      return Rollback(std::move(E));
    if (auto E = CPU.write(*Replacement + Offset, Chunk))
      return Rollback(std::move(E));
    Offset += Chunk.size();
  }
  if (auto E = Memory.unmap(Address, Old.MappedSize))
    return Rollback(std::move(E));
  Allocations.erase(Found);
  return *Replacement;
}

llvm::Expected<std::optional<uint64_t>>
Services::heap(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  if (A[0] != HeapHandle)
    return unsupported(S);
  const uint32_t Flags = A[1];
  uint32_t Allowed = HeapNoSerialize;
  if (S.Kind == API::HeapAlloc || S.Kind == API::HeapReAlloc)
    Allowed |= HeapZeroMemory;
  if (S.Kind == API::HeapReAlloc)
    Allowed |= HeapReallocInPlaceOnly;
  if (Flags & ~Allowed)
    return unsupported(S);
  auto Found = Allocations.find(A[2]);
  if (S.Kind != API::HeapAlloc && Found != Allocations.end() &&
      Found->second.EnvironmentSnapshot)
    return unsupported(S);
  if (S.Kind == API::HeapSize)
    return std::optional<uint64_t>(
        Found == Allocations.end() ? UINT64_MAX : Found->second.Size);
  if (S.Kind == API::HeapFree) {
    if (!A[2])
      return std::optional<uint64_t>(1);
    if (Found == Allocations.end())
      return std::optional<uint64_t>(0);
    if (auto E = Memory.unmap(Found->first, Found->second.MappedSize))
      return std::move(E);
    Allocations.erase(Found);
    return std::optional<uint64_t>(1);
  }
  if (S.Kind == API::HeapReAlloc && Found == Allocations.end())
    return unsupported(S);
  auto Address = S.Kind == API::HeapAlloc ? allocateHeap(A[2])
                                          : reallocateHeap(A[2], A[3], Flags);
  if (!Address)
    return Address.takeError();
  return std::optional<uint64_t>(*Address);
}
} // namespace neverd::emulation::windows_process
