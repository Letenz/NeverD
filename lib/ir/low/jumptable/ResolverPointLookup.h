//===- ResolverPointLookup.h - Frozen proof-point lookups -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_JUMPTABLE_RESOLVERPOINTLOOKUP_H
#define NEVERD_IR_LOW_JUMPTABLE_RESOLVERPOINTLOOKUP_H

#include "neverd/ir/low/LowIR.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace neverd::detail {

/// One immutable graph owns this bounded lookup table. Its index must not
/// change after the first lookup. The complete address/sequence key is checked
/// on every hit; collisions only cause another ordinary ordered lookup.
/// Locations are returned by value so a subsequent colliding query cannot
/// invalidate a caller's result. Callers still pay the original lookup charge.
class ResolverPointLookup {
  using Point = std::pair<va_t, int>;
  using Location = std::pair<int, int>;
  struct Entry {
    Point Key{};
    Location Value{};
    bool Valid = false;
    bool Present = false;
  };
  static constexpr size_t Capacity = 256;
  using Table = std::array<Entry, Capacity>;
  static_assert(sizeof(Table) <= 8 * 1024);
  const void *Owner = nullptr;
  std::unique_ptr<Table> Entries;
  unsigned UncachedLookups = 0;

public:
  template <typename Index>
  std::optional<Location> find(const Index &Points, Point Key) {
    auto lookup = [&]() -> std::optional<Location> {
      const auto It = Points.find(Key);
      return It == Points.end() ? std::nullopt
                                : std::optional<Location>(It->second);
    };
    // Small graphs have cheap ordered searches and need no retained table.
    if (Points.size() < 64)
      return lookup();
    if (Owner != &Points) {
      Entries.reset();
      UncachedLookups = 0;
      Owner = &Points;
    }
    // A large graph can still receive just a handful of point queries. Wait
    // for repeated use before paying for the fixed table's initialization.
    if (!Entries) {
      if (++UncachedLookups < 64)
        return lookup();
      Entries = std::make_unique<Table>();
    }
    const uint64_t Address = Key.first;
    const uint64_t Sequence = static_cast<uint32_t>(Key.second);
    const size_t Slot = (Address ^ (Address >> 4) ^ (Address >> 16) ^
                         (Address >> 32) ^ (Sequence * 2654435761U)) %
                        Capacity;
    auto &Saved = (*Entries)[Slot];
    if (Saved.Valid && Saved.Key == Key)
      return Saved.Present ? std::optional<Location>(Saved.Value)
                           : std::nullopt;
    const auto Found = lookup();
    Saved = {Key, Found.value_or(Location{}), true, Found.has_value()};
    return Found;
  }
};

} // namespace neverd::detail

#endif
