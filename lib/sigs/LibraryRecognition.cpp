//===- LibraryRecognition.cpp - Shared evidence conflict policy ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/LibraryRecognition.h"

#include <algorithm>
#include <set>

using namespace neverd::sigs;

bool neverd::sigs::finalizeLibraryRecognitions(
    std::vector<LibraryRecognition> &Matches, size_t &Work) {
  auto Spend = [&] {
    if (!Work) {
      Matches.clear();
      return false;
    }
    --Work;
    return true;
  };
  if (Matches.size() > 128) {
    Matches.clear();
    return false;
  }
  std::set<std::pair<std::string, std::string>> WholeRules;
  for (const auto &M : Matches)
    if (M.Scope == LibraryFeatureScope::WholeFunction)
      WholeRules.emplace(M.Pack, M.Rule);
  std::erase_if(Matches, [&](const auto &M) {
    return M.Scope != LibraryFeatureScope::WholeFunction &&
           WholeRules.contains({M.Pack, M.Rule});
  });
  std::set<size_t> Conflicts;
  for (size_t I = 0; I < Matches.size(); ++I)
    for (size_t J = I + 1; J < Matches.size(); ++J) {
      if (!Spend())
        return false;
      auto &A = Matches[I];
      auto &B = Matches[J];
      bool Overlap = false;
      for (const auto &Occurrence : A.Occurrences) {
        if (!Spend())
          return false;
        if (std::binary_search(B.Occurrences.begin(), B.Occurrences.end(),
                               Occurrence)) {
          Overlap = true;
          break;
        }
      }
      if (!Overlap)
        continue;
      if (A.Pack != B.Pack) {
        Conflicts.insert(I);
        Conflicts.insert(J);
      } else if (A.Occurrences.size() != B.Occurrences.size()) {
        auto &Inner = A.Occurrences.size() < B.Occurrences.size() ? A : B;
        auto &Outer = A.Occurrences.size() < B.Occurrences.size() ? B : A;
        // A checked byte body can own its base-class accessor. Structural
        // nesting requires the same receiver type as the containing proof.
        if (Outer.Isolated &&
            (Outer.ByteLength || A.ReceiverType == B.ReceiverType) &&
            std::includes(Outer.Occurrences.begin(), Outer.Occurrences.end(),
                          Inner.Occurrences.begin(), Inner.Occurrences.end())) {
          Inner.Isolated = false;
          continue;
        }
      }
      A.Isolated = B.Isolated = false;
    }
  size_t Index = 0;
  std::erase_if(Matches,
                [&](const auto &) { return Conflicts.contains(Index++); });
  std::sort(Matches.begin(), Matches.end(), [](const auto &A, const auto &B) {
    return std::tie(A.Function, A.Occurrences, A.Pack, A.Rule) <
           std::tie(B.Function, B.Occurrences, B.Pack, B.Rule);
  });
  return true;
}
