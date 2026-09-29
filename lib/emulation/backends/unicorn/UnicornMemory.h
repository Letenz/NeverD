//===- UnicornMemory.h - Contiguous RAM projection ranges ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_UNICORNMEMORY_H
#define NEVERD_EMULATION_UNICORNMEMORY_H
#include "../../core/MemoryLayout.h"
#include "../../core/MemoryProjection.h"

namespace neverd::emulation {
/// Batch adjacent pages without changing mapping or allocation ownership.
/// The caller retains page pins through transport unmap and destruction.
template <typename Function>
llvm::Error
forEachUnicornRAMRange(const std::map<uint64_t, MemoryProjection::Page> &Pages,
                       Function Apply) {
  auto I = Pages.begin();
  while (I != Pages.end()) {
    const auto [Address, Page] = *I++;
    if (Page.IO)
      continue;
    uint64_t Size = memory::PageSize;
    while (I != Pages.end() && !I->second.IO && I->first - Address == Size &&
           I->second.Physical - Page.Physical == Size &&
           I->second.Permissions == Page.Permissions) {
      Size += memory::PageSize;
      ++I;
    }
    if (auto E = Apply(Address, Size, Page))
      return E;
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
#endif
