//===- RegistrationStateSEHDispatch.cpp - SEH exceptional root domains ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Collects every reaching level before constructing a shared callback root.

#include "RegistrationStateSolver.h"

#include "neverd/loader/RegistrationScopeGraph.h"

#include "llvm/ADT/SmallBitVector.h"

#include <bit>
#include <climits>

namespace neverd::registration_state {

void RegistrationStateSolver::dispatchSEHScopes(
    const Domain &Before, bool Invalid, const std::set<int32_t> &Levels) {
  if (Levels.empty() || *Levels.rbegin() < 0)
    return;
  const size_t Count = size_t(*Levels.rbegin()) + 1;
  if (Count > Chain.Scopes.size() || !charge(Count + Levels.size() + 1))
    return;
  // Bit zero denotes the frame sentinel; all scope levels use index + 1.
  // Scope admission already proved that each enclosing link precedes its row.
  const size_t Bits = Count + 1;
  const size_t Words = (Bits + sizeof(uintptr_t) * CHAR_BIT - 1) /
                       (sizeof(uintptr_t) * CHAR_BIT);
  std::vector<llvm::SmallBitVector> Origins(Count);
  for (int32_t Level : Levels) {
    if (Level < 0)
      continue;
    if (!charge(Words + 1))
      return;
    Origins[Level].resize(Bits);
    Origins[Level].set(size_t(Level) + 1);
  }

  using Key = std::tuple<va_t, bool, bool>;
  std::map<Key, llvm::SmallBitVector> Roots;
  auto GetRoot = [&](va_t Address, bool Callback,
                     bool Filter) -> llvm::SmallBitVector * {
    if (!charge(1 + 2 * std::bit_width(Roots.size() + 1)) ||
        !Entries.count(Address))
      return nullptr;
    auto [It, New] = Roots.try_emplace({Address, Callback, Filter});
    if (New) {
      if (!charge(Words + 1))
        return nullptr;
      It->second.resize(Bits);
    }
    return &It->second;
  };
  for (size_t I = Count; I-- != 0;) {
    if (!charge(1))
      return;
    if (Origins[I].empty())
      continue;
    auto Current = std::move(Origins[I]);
    const auto &Scope = Chain.Scopes[I];
    if (auto *Filter = GetRoot(Scope.FilterVA, true, true)) {
      if (!charge(Words + 1))
        return;
      *Filter |= Current;
    }
    if (auto *Handler = GetRoot(Scope.HandlerVA, Scope.IsFinally, false))
      Handler->set(Scope.EnclosingLevel < 0 ? 0 : Scope.EnclosingLevel + 1);
    if (Exhausted)
      return;
    if (Scope.EnclosingLevel < 0)
      continue;
    auto &Outer = Origins[Scope.EnclosingLevel];
    if (Outer.empty())
      Outer = std::move(Current);
    else {
      if (!charge(Words + 1))
        return;
      Outer |= Current;
    }
  }
  for (const auto &[Identity, Domain] : Roots) {
    if (!charge(Words + 1))
      return;
    std::set<int32_t> RootLevels;
    for (int Bit = Domain.find_first(); Bit >= 0; Bit = Domain.find_next(Bit)) {
      if (!charge(1 + 2 * std::bit_width(RootLevels.size() + 1)))
        return;
      RootLevels.insert(Bit ? Bit - 1 : registrationOutsideTryLevel(EH4));
    }
    const auto &[Address, Callback, Filter] = Identity;
    dispatch(Address, std::move(RootLevels), Before, Invalid, Callback, Filter);
  }
}

} // namespace neverd::registration_state
