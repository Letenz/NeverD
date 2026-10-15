//===- RAMInitializationWatch.h - Committed field writes ---------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Internal physical write markers, independent of access permission checks.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_CORE_RAMINITIALIZATIONWATCH_H
#define NEVERD_EMULATION_CORE_RAMINITIALIZATIONWATCH_H

#include "neverd/emulation/MemoryView.h"

#include <atomic>

namespace neverd::emulation {
/// Retain exact physical slices and an owning initialization bitmap. Only
/// committed writes mark bytes; opaque and pre-write observations do not.
/// Metadata queries may run in a guest-access validator without reading RAM.
/// No domain callback or borrowed model-storage pointer enters the RAM owner.
class RAMInitializationWatch final {
public:
  static llvm::Expected<std::shared_ptr<RAMInitializationWatch>>
  create(const MemoryView &Bytes);
  bool initialized(uint64_t Offset) const;

private:
  friend class PhysicalMemory;
  friend class MemoryProjection;
  struct Part {
    uint64_t Physical, Size, Offset;
  };
  MemoryView Bytes;
  std::unique_ptr<std::atomic<bool>[]> Flags;
  std::vector<Part> Parts;
  void committed(uint64_t Physical, uint64_t Size);
};
} // namespace neverd::emulation
#endif
