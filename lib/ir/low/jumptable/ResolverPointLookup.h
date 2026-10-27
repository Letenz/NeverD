//===- ResolverPointLookup.h - Frozen proof-point lookups -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_JUMPTABLE_RESOLVERPOINTLOOKUP_H
#define NEVERD_IR_LOW_JUMPTABLE_RESOLVERPOINTLOOKUP_H

#include "neverd/ir/Limits.h"
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
    va_t Address = 0;
    int Sequence = 0;
    bool Valid = false;
    bool Present = false;
    Location Value{};
  };
  static constexpr size_t Ways = 4;
  static constexpr size_t BucketCount = 128;
  struct Table {
    std::array<Entry, BucketCount * Ways> Values{};
    std::array<uint8_t, BucketCount> NextVictim{};
  };
  static_assert(sizeof(Table) <= 13 * 1024);
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
    if (Points.size() <
        ::neverd::limits::ir::kResolverPointLookupFindPointsSizeLimit)
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
    const size_t Bucket = (Address ^ (Address >> 4) ^ (Address >> 16) ^
                           (Address >> 32) ^ (Sequence * 2654435761U)) %
                          BucketCount;
    Entry *Saved = nullptr;
    for (size_t I = 0; I < Ways; ++I) {
      auto &Candidate = Entries->Values[Bucket * Ways + I];
      // Buckets fill in order, and eviction never creates an empty slot.
      if (!Candidate.Valid) {
        Saved = &Candidate;
        break;
      }
      if (Candidate.Address == Key.first && Candidate.Sequence == Key.second)
        return Candidate.Present ? std::optional<Location>(Candidate.Value)
                                 : std::nullopt;
    }
    const auto Found = lookup();
    if (!Saved) {
      auto &Next = Entries->NextVictim[Bucket];
      Saved = &Entries->Values[Bucket * Ways + Next];
      Next = static_cast<uint8_t>((Next + 1) % Ways);
    }
    *Saved = {Key.first, Key.second, true, Found.has_value(),
              Found.value_or(Location{})};
    return Found;
  }
};

} // namespace neverd::detail

#endif
