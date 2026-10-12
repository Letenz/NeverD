//===- RAMInitializationWatch.cpp - Exact committed field writes ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Publish initialization only after the RAM owner commits the actual bytes.
//===----------------------------------------------------------------------===//
#include "RAMInitializationWatch.h"

#include "ExecutionDiagnostics.h"
#include "MemoryLayout.h"
#include "MemoryStorage.h"

#include <algorithm>

namespace neverd::emulation {
llvm::Expected<std::shared_ptr<RAMInitializationWatch>>
RAMInitializationWatch::create(const MemoryView &Bytes) {
  if (auto E = Bytes.validate(0, Bytes.size()))
    return E;
  if (!Bytes.size())
    return diagnostic::error(diagnostic::MemoryRegion);
  auto &RAM = *Bytes.Memory->State;
  std::unique_lock Lock(RAM.Mutex, std::try_to_lock);
  if (!Lock.owns_lock() || RAM.Running)
    return diagnostic::error(diagnostic::Running);
  auto Watch = std::make_shared<RAMInitializationWatch>();
  Watch->Bytes = Bytes;
  Watch->Flags = std::make_unique<std::atomic<bool>[]>(Bytes.size());
  for (uint64_t I = 0; I < Bytes.size(); ++I)
    Watch->Flags[I].store(false, std::memory_order_relaxed);
  uint64_t Offset = 0;
  for (const auto &Slice : Bytes.Slices) {
    Watch->Parts.push_back(
        {memory::ProjectionReserve + Slice.Region->Offset + Slice.Offset,
         Slice.Size, Offset});
    Offset += Slice.Size;
  }
  std::erase_if(RAM.InitializationWatches,
                [](const auto &Weak) { return Weak.expired(); });
  RAM.InitializationWatches.push_back(Watch);
  return Watch;
}

bool RAMInitializationWatch::initialized(uint64_t Offset) const {
  return Offset < Bytes.size() && Flags[Offset].load(std::memory_order_acquire);
}

void RAMInitializationWatch::committed(uint64_t Physical, uint64_t Size) {
  for (const auto &P : Parts) {
    const uint64_t Begin = std::max(Physical, P.Physical);
    if (Begin - Physical >= Size || Begin - P.Physical >= P.Size)
      continue;
    const uint64_t Count =
        std::min(Size - (Begin - Physical), P.Size - (Begin - P.Physical));
    for (uint64_t I = 0; I < Count; ++I)
      Flags[P.Offset + Begin - P.Physical + I].store(true,
                                                     std::memory_order_release);
  }
}

void PhysicalMemory::Impl::recordInitialization(uint64_t Physical,
                                                uint64_t Size) {
  if (!Size)
    return;
  std::erase_if(InitializationWatches, [&](const auto &Weak) {
    auto Watch = Weak.lock();
    if (!Watch)
      return true;
    Watch->committed(Physical, Size);
    return false;
  });
}
} // namespace neverd::emulation
