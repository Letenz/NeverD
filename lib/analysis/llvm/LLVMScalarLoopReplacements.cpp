//===- LLVMScalarLoopReplacements.cpp - Proved substitution batches ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMScalarLoopRecoveryInternal.h"

namespace neverd::analysis::scalar_recovery {
using namespace llvm;

void substitute(Candidate &C, ArrayRef<Replacement> Replacements) {
  for (auto R : Replacements) {
    auto *Target = C.get(R.Target);
    Target->replaceAllUsesWith(C.map(R.Preferred));
    Target->eraseFromParent();
  }
}

bool proposeReplacements(Search &S, Function &F,
                         ArrayRef<Replacement> Replacements) {
  if (Replacements.empty())
    return false;
  auto Try = [&](ArrayRef<Replacement> Choices) {
    Candidate C;
    if (!S.clone(F, C))
      return false;
    substitute(C, Choices);
    // Passing individual rejection-only screens proves neither a slot nor
    // their composition. Acceptance still needs complete symbolic data.
    return S.accept(C);
  };
  if (Try(Replacements))
    return true;
  if (S.stopped())
    return false;
  if (Replacements.size() == 1) {
    auto R = Replacements.front();
    if (R.Alternative && Try({{R.Target, R.Alternative}}))
      return true;
    if (!S.stopped())
      S.deferReplacement(*R.Target);
    return false;
  }
  const unsigned Mid = Replacements.size() / 2;
  if (proposeReplacements(S, F, Replacements.take_front(Mid)))
    return true;
  if (S.stopped())
    return false;
  return proposeReplacements(S, F, Replacements.drop_front(Mid));
}
} // namespace neverd::analysis::scalar_recovery
