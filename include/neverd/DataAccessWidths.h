//===- DataAccessWidths.h - Widths that name accessed data ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The width that names data the code reads or writes, which the listing and
/// the decompiled C share so that a reader finds one name for an address in
/// both (neverd/DataNames.def): the width every read and write the code states
/// at the address shares, or none where two differ, which leaves the data
/// unknown (`unk_`).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DATAACCESSWIDTHS_H
#define NEVERD_DATAACCESSWIDTHS_H

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace neverd {

/// The widest access that names data; a wider one, such as a whole saved
/// register area, states no width a name takes.
inline constexpr unsigned MaxDataAccessWidth = 64;

/// The width of the reads and writes at each address the code states.
class DataAccessWidths {
public:
  /// Takes a read or write of \p Width bytes at \p Address.  Accesses arrive
  /// in address order; one of no width, or wider than MaxDataAccessWidth,
  /// says nothing.
  void add(uint64_t Address, unsigned Width) {
    if (!Width || Width > MaxDataAccessWidth)
      return;
    if (Widths.empty() || Widths.back().first != Address)
      Widths.emplace_back(Address, Width);
    else if (Widths.back().second != Width)
      Widths.back().second = 0;
  }

  /// The width every access to \p Address shares, 0 where they differ, or
  /// none when the code reads and writes no data there.
  std::optional<unsigned> at(uint64_t Address) const {
    const auto It = std::lower_bound(
        Widths.begin(), Widths.end(), Address,
        [](const auto &Entry, uint64_t Value) { return Entry.first < Value; });
    if (It == Widths.end() || It->first != Address)
      return std::nullopt;
    return It->second;
  }

  /// Each address and its width, in address order.
  auto begin() const { return Widths.begin(); }
  auto end() const { return Widths.end(); }
  bool empty() const { return Widths.empty(); }
  void clear() { Widths.clear(); }

private:
  std::vector<std::pair<uint64_t, unsigned>> Widths;
};

} // namespace neverd

#endif // NEVERD_DATAACCESSWIDTHS_H
