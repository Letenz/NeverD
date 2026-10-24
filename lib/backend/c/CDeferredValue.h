//===- CDeferredValue.h - Unknown scalar transport in C -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A PHI edge transports a value; it does not observe it. Carry availability
/// alongside mixed known/unknown PHIs and their scalar copies. Only a later
/// value read diagnoses missing machine state. Both C projections use this
/// finite worklist, including cycles and copies preceding their definitions.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_CDEFERREDVALUE_H
#define NEVERD_BACKEND_C_CDEFERREDVALUE_H

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {

template <typename Key> struct CValueTransfer {
  Key Destination;
  std::optional<Key> Source;
  bool UnknownPhiInput = false;
};

template <typename Key>
std::set<Key>
deferredCValues(const std::vector<CValueTransfer<Key>> &Transfers) {
  std::set<Key> Deferred;
  std::vector<Key> Work;
  for (const auto &Transfer : Transfers) {
    if (Transfer.UnknownPhiInput &&
        Deferred.insert(Transfer.Destination).second)
      Work.push_back(Transfer.Destination);
  }
  if (Work.empty())
    return Deferred;
  std::map<Key, std::vector<Key>> Users;
  for (const auto &Transfer : Transfers)
    if (Transfer.Source)
      Users[*Transfer.Source].push_back(Transfer.Destination);
  while (!Work.empty()) {
    const Key Value = Work.back();
    Work.pop_back();
    const auto Found = Users.find(Value);
    if (Found == Users.end())
      continue;
    for (const Key &User : Found->second)
      if (Deferred.insert(User).second)
        Work.push_back(User);
  }
  return Deferred;
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_CDEFERREDVALUE_H
