//===- RegistrationScopeGraph.h - x86 SEH graph validation ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Shares EH3/EH4 scope-index and normalized record validation.

#ifndef NEVERD_LOADER_REGISTRATIONSCOPEGRAPH_H
#define NEVERD_LOADER_REGISTRATIONSCOPEGRAPH_H

#include "neverd/loader/LanguageEHRegistration.h"

#include <limits>

namespace neverd {

inline constexpr int32_t registrationOutsideTryLevel(bool IsEH4) {
  return IsEH4 ? -2 : -1;
}

/// AvailableLevels is exclusive: a current state uses the whole table count,
/// while an enclosing link uses only records preceding its own row. A local
/// unwind's requested numeric bound is not a scope index.
inline constexpr bool isValidRegistrationScopeLevel(int32_t Level,
                                                    uint32_t AvailableLevels,
                                                    bool IsEH4) {
  return Level == registrationOutsideTryLevel(IsEH4) ||
         (Level >= 0 && uint32_t(Level) < AvailableLevels);
}

/// This validates normalized records independently of a parser status. Code
/// bytes, executable address ownership and frame installation have separate
/// producers. Scope code addresses use the PE32 pointer domain; an empty table
/// can retain only its outside-frame state.
inline bool hasValidRegistrationScopeGraph(const RegistrationChainInfo &Chain,
                                           bool IsEH4) {
  if (!Chain.SeededTryLevel ||
      Chain.Scopes.size() > std::numeric_limits<uint32_t>::max() ||
      !isValidRegistrationScopeLevel(*Chain.SeededTryLevel,
                                     uint32_t(Chain.Scopes.size()), IsEH4))
    return false;
  for (uint32_t I = 0; I != Chain.Scopes.size(); ++I) {
    const auto &Scope = Chain.Scopes[I];
    if (!isValidRegistrationScopeLevel(Scope.EnclosingLevel, I, IsEH4) ||
        !Scope.HandlerVA || Scope.HandlerVA > UINT32_MAX ||
        Scope.FilterVA > UINT32_MAX || Scope.IsFinally != (Scope.FilterVA == 0))
      return false;
  }
  return true;
}

} // namespace neverd

#endif
