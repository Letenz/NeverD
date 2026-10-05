//===- RAMReservation.cpp - Physical write witnesses
//-----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "MemoryStorage.h"

#include <algorithm>

namespace neverd::emulation {
void PhysicalMemory::Impl::invalidateReservations(uint64_t Physical,
                                                  uint64_t Size) {
  if (!Size)
    return;
  std::erase_if(Reservations, [&](const auto &Weak) {
    auto R = Weak.lock();
    if (!R)
      return true;
    const bool Overlap = Physical <= R->Granule
                             ? R->Granule - Physical < Size
                             : Physical - R->Granule < R->GranuleSize;
    if (Overlap)
      R->Valid = false;
    return !R->Valid;
  });
}
void PhysicalMemory::Impl::invalidateReservations() {
  for (const auto &Weak : Reservations)
    if (auto R = Weak.lock())
      R->Valid = false;
  Reservations.clear();
}
} // namespace neverd::emulation
