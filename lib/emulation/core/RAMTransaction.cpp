//===- RAMTransaction.cpp - Commit or discard declared RAM effects --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "RAMTransaction.h"

#include "ExecutionDiagnostics.h"
#include "MemoryLayout.h"

#include <algorithm>
#include <cstring>

namespace neverd::emulation {
RAMTransaction::RAMTransaction(MemoryProjection &Memory,
                               std::unique_lock<std::recursive_mutex> Lease,
                               std::vector<Slice> Slices)
    : Memory(Memory), Lease(std::move(Lease)), Slices(std::move(Slices)) {}

RAMTransaction::~RAMTransaction() {
  if (State == Phase::Executing)
    restore();
}

llvm::Expected<std::unique_ptr<RAMTransaction>>
RAMTransaction::create(MemoryProjection &Memory,
                       llvm::ArrayRef<RAMWriteRange> Writes,
                       uint64_t ByteBudget, unsigned Permissions) {
  auto Lease = Memory.executionLock();
  if (!Lease)
    return Lease.takeError();
  if (!ByteBudget || ByteBudget > memory::MaxRAM ||
      Writes.size() > ByteBudget || !(Permissions & Write))
    return diagnostic::error(diagnostic::RAMTransactionBudget);
  struct Range {
    uint64_t Physical, Size;
  };
  std::vector<Range> Ranges;
  for (const auto &W : Writes) {
    if (!W.Size || W.Size > ByteBudget || W.Size - 1 > UINT64_MAX - W.Address)
      return diagnostic::error(diagnostic::RAMTransactionRange);
    uint64_t Address = W.Address, Remaining = W.Size;
    while (Remaining) {
      const uint64_t Offset = Address % memory::PageSize;
      const auto P = Memory.mappings().find(Address - Offset);
      if (P == Memory.mappings().end() || P->second.IO ||
          (P->second.Permissions & Permissions) != Permissions)
        return diagnostic::error(diagnostic::RAMTransactionRange);
      const uint64_t Size = std::min(Remaining, memory::PageSize - Offset);
      Ranges.push_back({P->second.Physical + Offset, Size});
      Address += Size;
      Remaining -= Size;
    }
  }
  std::sort(Ranges.begin(), Ranges.end(), [](const Range &A, const Range &B) {
    return A.Physical < B.Physical;
  });
  // Virtual aliases may overlap or repeat the same physical bytes. Snapshot
  // their union once so rollback cannot resurrect an intermediate value.
  std::vector<Range> Merged;
  for (const auto &R : Ranges) {
    if (!Merged.empty() &&
        R.Physical - Merged.back().Physical <= Merged.back().Size) {
      auto &Last = Merged.back();
      Last.Size = std::max(Last.Size, R.Physical - Last.Physical + R.Size);
    } else
      Merged.push_back(R);
  }
  uint64_t Total = 0;
  for (const auto &R : Merged) {
    if (R.Size > ByteBudget - Total)
      return diagnostic::error(diagnostic::RAMTransactionBudget);
    Total += R.Size;
  }
  // Allocate both images before host entry. Neither stage nor commit allocates
  // memory, calls guest observers, or changes the mapping authority.
  std::vector<Slice> Slices;
  Slices.reserve(Merged.size());
  for (const auto &R : Merged) {
    Slice S{R.Physical, std::vector<uint8_t>(R.Size),
            std::vector<uint8_t>(R.Size)};
    std::memcpy(S.Before.data(), Memory.physicalPointer(R.Physical), R.Size);
    Slices.push_back(std::move(S));
  }
  return std::unique_ptr<RAMTransaction>(
      new RAMTransaction(Memory, std::move(*Lease), std::move(Slices)));
}

void RAMTransaction::restore() {
  for (const auto &S : Slices)
    std::memcpy(Memory.physicalPointer(S.Physical), S.Before.data(),
                S.Before.size());
}

llvm::Error RAMTransaction::stage() {
  if (State != Phase::Executing)
    return diagnostic::error(diagnostic::RAMTransactionPhase);
  for (auto &S : Slices)
    std::memcpy(S.After.data(), Memory.physicalPointer(S.Physical),
                S.After.size());
  restore();
  State = Phase::Staged;
  return llvm::Error::success();
}

llvm::Error RAMTransaction::read(uint64_t Address,
                                 llvm::MutableArrayRef<uint8_t> Bytes) const {
  if (State != Phase::Staged)
    return diagnostic::error(diagnostic::RAMTransactionPhase);
  if (Bytes.empty() || Bytes.size() - 1 > UINT64_MAX - Address)
    return diagnostic::error(diagnostic::RAMTransactionRange);
  // Validate the whole read before publishing any bytes to the caller.
  auto Transfer = [&](bool Copy) -> llvm::Error {
    uint64_t Current = Address;
    size_t Copied = 0;
    while (Copied < Bytes.size()) {
      const uint64_t Offset = Current % memory::PageSize;
      auto P = Memory.mappings().find(Current - Offset);
      if (P == Memory.mappings().end() || P->second.IO)
        return diagnostic::error(diagnostic::RAMTransactionRange);
      const uint64_t Physical = P->second.Physical + Offset;
      auto S = std::upper_bound(
          Slices.begin(), Slices.end(), Physical,
          [](uint64_t A, const Slice &S) { return A < S.Physical; });
      if (S == Slices.begin())
        return diagnostic::error(diagnostic::RAMTransactionRange);
      --S;
      const uint64_t InSlice = Physical - S->Physical;
      if (InSlice >= S->After.size())
        return diagnostic::error(diagnostic::RAMTransactionRange);
      const uint64_t Size =
          std::min({uint64_t(Bytes.size() - Copied), memory::PageSize - Offset,
                    uint64_t(S->After.size() - InSlice)});
      if (Copy)
        std::memcpy(Bytes.data() + Copied, S->After.data() + InSlice, Size);
      Copied += Size;
      Current += Size;
    }
    return llvm::Error::success();
  };
  if (auto E = Transfer(false))
    return E;
  return Transfer(true);
}

llvm::Error RAMTransaction::commit() {
  if (State != Phase::Staged)
    return diagnostic::error(diagnostic::RAMTransactionPhase);
  for (const auto &S : Slices)
    std::memcpy(Memory.physicalPointer(S.Physical), S.After.data(),
                S.After.size());
  State = Phase::Committed;
  return llvm::Error::success();
}
} // namespace neverd::emulation
