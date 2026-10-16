//===- MedCallLeftovers.cpp - Values calls leave in registers -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedCallLeftovers.h"

#include "llvm/ADT/STLExtras.h"

#include <map>
#include <vector>

namespace neverd {
namespace {

std::tuple<int, int, int> valueKey(const MedVar &V) {
  return {static_cast<int>(V.Kind), V.Id, V.SSAVer};
}

} // namespace

ValuesLeftByCalls::ValuesLeftByCalls(const MedFunc &Func) {
  std::set<std::tuple<int, int, int>> ResultFields;
  for (const MedStructReturnCandidate &Candidate : Func.StructReturnCandidates)
    for (const MedVar &Field : Candidate.Fields)
      ResultFields.insert(valueKey(Field));
  for (const MedCallClobber &Clobber : Func.CallClobbers)
    if (Clobber.Value.Kind == MedVar::Reg && Clobber.PreservedPrefixSize == 0 &&
        !ResultFields.count(valueKey(Clobber.Value)))
      Values.insert(valueKey(Clobber.Value));
  if (Values.empty())
    return;

  // A join holds only what calls left where everything reaching it does.
  // Take every join for one, then drop each that anything else reaches, and
  // each that a dropped join reaches: the joins that remain, loops of them
  // included, are all such joins.
  std::set<std::tuple<int, int, int>> Joins;
  std::vector<const PhiNode *> Phis;
  for (const MedBlock &Block : Func.Blocks)
    for (const PhiNode &Phi : Block.Phis)
      if (Phi.Output.Kind == MedVar::Reg) {
        Joins.insert(valueKey(Phi.Output));
        Phis.push_back(&Phi);
      }
  std::map<std::tuple<int, int, int>, std::vector<const PhiNode *>> Readers;
  std::vector<const PhiNode *> Dropped;
  for (const PhiNode *Phi : Phis) {
    bool ReachedByOther = Phi->Args.empty();
    auto reachedBy = [&](const MedVar &Incoming) {
      const auto Key = valueKey(Incoming);
      Readers[Key].push_back(Phi);
      ReachedByOther |= !Values.count(Key) && !Joins.count(Key);
    };
    for (const auto &[Pred, Incoming] : Phi->Args)
      reachedBy(Incoming);
    // The exception dispatcher's entry is one more path.
    if (Phi->ExceptionalEntry)
      reachedBy(*Phi->ExceptionalEntry);
    if (ReachedByOther)
      Dropped.push_back(Phi);
  }
  while (!Dropped.empty()) {
    const PhiNode *Phi = Dropped.back();
    Dropped.pop_back();
    const auto Key = valueKey(Phi->Output);
    if (!Joins.erase(Key))
      continue;
    if (const auto It = Readers.find(Key); It != Readers.end())
      llvm::append_range(Dropped, It->second);
  }
  Values.insert(Joins.begin(), Joins.end());
}

bool ValuesLeftByCalls::contains(const MedVar &V) const {
  return V.Kind == MedVar::Reg && Values.count(valueKey(V));
}

} // namespace neverd
