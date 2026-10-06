//===- MemoryView.cpp - Access through retained allocation identity -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/MemoryView.h"

#include "ExecutionDiagnostics.h"
#include "MemoryLayout.h"
#include "MemoryStorage.h"

#include <algorithm>
#include <cstring>

namespace neverd::emulation {
MemoryView::MemoryView(std::shared_ptr<PhysicalMemory> Memory,
                       std::shared_ptr<const void> Space, uint64_t Size,
                       std::vector<MemorySlice> Slices)
    : Memory(std::move(Memory)), Space(std::move(Space)), Size(Size),
      Slices(std::move(Slices)) {}
llvm::Error MemoryView::validate(uint64_t Offset, uint64_t Length) const {
  if (!Memory || Offset > Size || Length > Size - Offset)
    return diagnostic::error(diagnostic::MemoryRegion);
  return llvm::Error::success();
}
llvm::Expected<MemoryView> MemoryView::subview(uint64_t Offset,
                                               uint64_t Length) const {
  if (auto E = validate(Offset, Length))
    return E;
  std::vector<MemorySlice> Parts;
  const uint64_t Total = Length;
  for (const auto &Slice : Slices) {
    if (!Length)
      break;
    if (Offset >= Slice.Size) {
      Offset -= Slice.Size;
      continue;
    }
    const uint64_t Count = std::min(Length, Slice.Size - Offset);
    Parts.push_back({Slice.Region, Slice.Offset + Offset, Count});
    Length -= Count;
    Offset = 0;
  }
  return MemoryView(Memory, Space, Total, std::move(Parts));
}
bool MemoryView::describesSameBytes(const MemoryView &Other) const {
  if (!Memory || Memory != Other.Memory || Size != Other.Size)
    return false;
  size_t A = 0, B = 0;
  uint64_t AOffset = 0, BOffset = 0;
  while (A != Slices.size() && B != Other.Slices.size()) {
    const auto &Left = Slices[A];
    const auto &Right = Other.Slices[B];
    if (Left.Region != Right.Region ||
        Left.Offset + AOffset != Right.Offset + BOffset)
      return false;
    const uint64_t Count = std::min(Left.Size - AOffset, Right.Size - BOffset);
    AOffset += Count;
    BOffset += Count;
    if (AOffset == Left.Size) {
      ++A;
      AOffset = 0;
    }
    if (BOffset == Right.Size) {
      ++B;
      BOffset = 0;
    }
  }
  return A == Slices.size() && B == Other.Slices.size();
}
bool MemoryView::overlaps(const MemoryView &Other) const {
  if (!Memory || Memory != Other.Memory)
    return false;
  for (const auto &Left : Slices)
    for (const auto &Right : Other.Slices)
      if (Left.Region == Right.Region &&
          (Left.Offset <= Right.Offset
               ? Right.Offset - Left.Offset < Left.Size
               : Left.Offset - Right.Offset < Right.Size))
        return true;
  return false;
}
llvm::Error MemoryView::validateAccess(uint64_t Offset, uint64_t Length) const {
  if (auto E = validate(Offset, Length))
    return E;
  auto &RAM = *Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || RAM.Running)
    return diagnostic::error(diagnostic::Running);
  return llvm::Error::success();
}
llvm::Error MemoryView::read(uint64_t Offset,
                             llvm::MutableArrayRef<uint8_t> Bytes) const {
  if (auto E = validate(Offset, Bytes.size()))
    return E;
  auto &RAM = *Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || RAM.Running)
    return diagnostic::error(diagnostic::Running);
  for (const auto &Slice : Slices) {
    if (Bytes.empty())
      break;
    if (Offset >= Slice.Size) {
      Offset -= Slice.Size;
      continue;
    }
    const auto *Backing = static_cast<const uint8_t *>(RAM.Backing.base()) +
                          Slice.Region->Offset + Slice.Offset + Offset;
    const size_t Count = std::min<uint64_t>(Bytes.size(), Slice.Size - Offset);
    std::memcpy(Bytes.data(), Backing, Count);
    Bytes = Bytes.drop_front(Count);
    Offset = 0;
  }
  return llvm::Error::success();
}
llvm::Error MemoryView::write(uint64_t Offset,
                              llvm::ArrayRef<uint8_t> Bytes) const {
  if (auto E = validate(Offset, Bytes.size()))
    return E;
  auto &RAM = *Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || RAM.Running)
    return diagnostic::error(diagnostic::Running);
  for (const auto &Slice : Slices) {
    if (Bytes.empty())
      break;
    if (Offset >= Slice.Size) {
      Offset -= Slice.Size;
      continue;
    }
    auto *Backing = static_cast<uint8_t *>(RAM.Backing.base()) +
                    Slice.Region->Offset + Slice.Offset + Offset;
    const size_t Count = std::min<uint64_t>(Bytes.size(), Slice.Size - Offset);
    RAM.invalidateReservations(memory::ProjectionReserve +
                                   Slice.Region->Offset + Slice.Offset + Offset,
                               Count);
    std::memcpy(Backing, Bytes.data(), Count);
    Bytes = Bytes.drop_front(Count);
    Offset = 0;
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
