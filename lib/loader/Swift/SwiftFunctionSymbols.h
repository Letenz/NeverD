#ifndef NEVERD_LOADER_SWIFT_SWIFTFUNCTIONSYMBOLS_H
#define NEVERD_LOADER_SWIFT_SWIFTFUNCTIONSYMBOLS_H

#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <vector>

namespace neverd {

// Index exact function symbols for one operation on an unchanged image.
// Rebuild after any symbol mutation; no cache is attached to BinaryImage.
// Exact identity queries keep equal-address rows ambiguous. A declaration
// classifier may separately require every repeated record to be identical.
class SwiftFunctionSymbolIndex {
public:
  explicit SwiftFunctionSymbolIndex(const BinaryImage &Image) : Owner(&Image) {
    Functions.reserve(Image.Symbols.size());
    for (const auto &Symbol : Image.Symbols)
      if (Symbol.IsFunc)
        Functions.push_back(&Symbol);
    std::sort(
        Functions.begin(), Functions.end(),
        [](const Symbol *A, const Symbol *B) { return A->Addr < B->Addr; });
  }

private:
  friend const Symbol *
  uniqueSwiftFunctionSymbol(const BinaryImage &, va_t,
                            const SwiftFunctionSymbolIndex *);
  friend const Symbol *
  consistentSwiftFunctionDeclarationSymbol(const BinaryImage &, va_t,
                                           const SwiftFunctionSymbolIndex *);

  const BinaryImage *Owner;
  std::vector<const Symbol *> Functions;
};

// A null or foreign index uses the live image. The raw address and IsFunc
// flag alone select candidates; names, exports and code ownership do not
// resolve a duplicate. ABI classifiers retain their own additional proofs.
inline const Symbol *
uniqueSwiftFunctionSymbol(const BinaryImage &Image, va_t Entry,
                          const SwiftFunctionSymbolIndex *Index = nullptr) {
  if (Index && Index->Owner == &Image) {
    const auto &Functions = Index->Functions;
    const auto Found =
        std::lower_bound(Functions.begin(), Functions.end(), Entry,
                         [](const Symbol *Candidate, va_t Address) {
                           return Candidate->Addr < Address;
                         });
    if (Found == Functions.end() || (*Found)->Addr != Entry ||
        (Found + 1 != Functions.end() && (*(Found + 1))->Addr == Entry))
      return nullptr;
    return *Found;
  }
  const Symbol *Only = nullptr;
  for (const auto &Candidate : Image.Symbols)
    if (Candidate.Addr == Entry && Candidate.IsFunc) {
      if (Only)
        return nullptr;
      Only = &Candidate;
    }
  return Only;
}

// A compiler declaration can encounter the same symbol in multiple loader
// records. Require every function record at the entry to agree in all fields;
// never choose between aliases, extents, provenance or guessed boundaries.
// This supplies a declaration candidate only. Current code, ABI transport and
// source publication must still prove the function body independently.
inline const Symbol *consistentSwiftFunctionDeclarationSymbol(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *Index = nullptr) {
  const Symbol *Only = nullptr;
  const auto Include = [&](const Symbol *Candidate) {
    if (!Only) {
      Only = Candidate;
      return true;
    }
    return Candidate->Name == Only->Name && Candidate->Addr == Only->Addr &&
           Candidate->Size == Only->Size && Candidate->IsFunc == Only->IsFunc &&
           Candidate->IsBoundaryGuess == Only->IsBoundaryGuess &&
           Candidate->Origin == Only->Origin;
  };
  if (Index && Index->Owner == &Image) {
    const auto &Functions = Index->Functions;
    auto Current = std::lower_bound(Functions.begin(), Functions.end(), Entry,
                                    [](const Symbol *Candidate, va_t Address) {
                                      return Candidate->Addr < Address;
                                    });
    for (; Current != Functions.end() && (*Current)->Addr == Entry; ++Current)
      if (!Include(*Current))
        return nullptr;
  } else {
    for (const auto &Candidate : Image.Symbols)
      if (Candidate.Addr == Entry && Candidate.IsFunc && !Include(&Candidate))
        return nullptr;
  }
  return Only;
}

} // namespace neverd

#endif
