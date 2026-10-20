//===- SymMBAProduct.cpp - Recovering a product from its expansion --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the search for a product of bitwise factors whose expansion over
/// the minterm basis is exactly the higher-degree part SymMBAPoly.cpp computed,
/// and the rewrite of a region as one such product plus a linear remainder.
///
/// See the theory note at the head of SymMBAPoly.cpp for why the expansion pins
/// a product down where the corner measurement cannot.
///
//===----------------------------------------------------------------------===//

#include "SymMBADetail.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <map>
#include <optional>
#include <utility>

namespace neverd::symbolic::detail {

namespace {

/// Multiply \p Factors out and count how often each monomial is reached, or
/// report that they cannot be the product being looked for.
///
/// Failing at the first monomial the target does not name is what keeps the
/// search far cheaper than its bound: almost every candidate is wrong, and
/// almost every wrong one is wrong on its first monomial.
bool expandProduct(llvm::ArrayRef<TruthTable> Factors,
                   const HigherCoefficients &Higher,
                   std::unordered_map<Monomial, unsigned, MonomialHash> &Counts,
                   WorkBudget &Budget) {
  Counts.clear();
  llvm::SmallVector<llvm::SmallVector<MintermIndex, 8>, 4> Selected;
  for (const TruthTable &Table : Factors) {
    Selected.push_back(selectedMinterms(Table));
    if (Selected.back().empty())
      return false;
  }

  bool Explains = true;
  forEachMonomial(Selected, Budget, [&](const Monomial &Key) {
    if (!Higher.count(Key)) {
      Explains = false;
      return false;
    }
    ++Counts[Key];
    return true;
  });
  // Reaching only monomials the target names is not enough; it has to reach all
  // of them, or the target holds something this product does not account for.
  return !Budget.exhausted() && Explains && Counts.size() == Higher.size();
}

/// Visit every product of \p Degree bitwise factors whose expansion is exactly
/// \p Higher, until \p Budget is spent.
///
/// The search is over factors rather than over all functions of the inputs,
/// because the target says a great deal about what the factors must be.  A
/// factor can only select a minterm the target names, and between them the
/// factors must name all of them; and a minterm repeated to the full degree is
/// a monomial exactly when *every* factor selects it, so those are known before
/// the search starts.  What is left to guess is a subset of the remainder,
/// which is what brings a degree-three product over three inputs -- the shape
/// polynomial obfuscation reaches for -- down from an unaffordable enumeration
/// to a few tens of thousands of candidates.
///
/// More than one product can match, and which reads best is not the order they
/// are found in, so the caller ranks every match reached within the budget.
/// Combination generation is iterative: product degree changes memory use, not
/// call-stack depth.
template <typename FnT>
void forEachProductMatch(const HigherCoefficients &Higher, unsigned Degree,
                         unsigned NumAtoms, uint32_t Width,
                         size_t MaxCandidates, WorkBudget &Budget, FnT Visit) {
  const std::optional<size_t> Corners = cornerCount(NumAtoms);
  if (!Corners || Degree < 2)
    return;

  TruthTable Active = TruthTable::zero(NumAtoms);
  for (const auto &[Key, Coeff] : Higher)
    Active |= monomialSupport(Key, NumAtoms);

  TruthTable Shared = TruthTable::zero(NumAtoms);
  for (size_t M = 0; M < *Corners; ++M) {
    Monomial Repeat(Degree, static_cast<MintermIndex>(M));
    if (Higher.count(Repeat))
      Shared.set(M);
  }

  llvm::SmallVector<TruthTable, 64> Candidates;
  const TruthTable Free = Active & ~Shared;
  for (TruthTable Sub = Free;; Sub = Sub.nextSubsetBelow(Free)) {
    TruthTable Table = Shared | Sub;
    if (!Table.isZero()) {
      if (!Budget.consume())
        return;
      // A candidate is a table of one bit per corner and the walk below is
      // over tuples of them, so a list nobody bounds is how a wide region
      // turns a search into an allocation failure rather than a long wait.
      if (Candidates.size() >= MaxCandidates)
        return;
      Candidates.push_back(std::move(Table));
    }
    if (Sub.isZero())
      break;
  }
  if (Candidates.empty())
    return;

  llvm::SmallVector<size_t, 4> Choice(Degree, 0);
  llvm::SmallVector<TruthTable, 4> Factors(Degree);
  std::unordered_map<Monomial, unsigned, MonomialHash> Counts;
  // Nondecreasing tuples, because a product does not care what order its
  // factors are written in.
  for (;;) {
    if (!Budget.consume())
      return;
    TruthTable Union = TruthTable::zero(NumAtoms);
    for (size_t I = 0; I < Degree; ++I) {
      Factors[I] = Candidates[Choice[I]];
      Union |= Factors[I];
    }

    if (Union == Active && expandProduct(Factors, Higher, Counts, Budget)) {
      // Pin the coefficient on any monomial the product reaches exactly once.
      // Where every monomial is reached more than once no single coefficient
      // can be read off. Every coefficient is checked below, so choosing a
      // different singleton cannot change whether a product matches.
      const llvm::APInt *Coeff = nullptr;
      for (const auto &[Key, Count] : Counts)
        if (Count == 1) {
          Coeff = &Higher.find(Key)->second;
          break;
        }
      bool Matches = Coeff && !Coeff->isZero();
      if (Matches) {
        for (const auto &[Key, Count] : Counts) {
          llvm::APInt Predicted =
              *Coeff * llvm::APInt(Width, Count, /*isSigned=*/false,
                                   /*implicitTrunc=*/true);
          if (Predicted != Higher.find(Key)->second) {
            Matches = false;
            break;
          }
        }
      }
      if (Matches && !Visit(*Coeff, Factors))
        return;
    }

    if (Budget.exhausted())
      return;

    size_t Pos = Degree;
    while (Pos > 0 && Choice[Pos - 1] + 1 == Candidates.size())
      --Pos;
    if (Pos == 0)
      return;
    const size_t Next = Choice[Pos - 1] + 1;
    for (size_t I = Pos - 1; I < Degree; ++I) {
      Choice[I] = Next;
    }
  }
}

} // namespace

std::optional<QuadraticProduct>
recoverQuadraticProduct(const HigherCoefficients &Higher, unsigned NumAtoms,
                        uint32_t Width, WorkBudget &Budget, size_t MaxBytes) {
  const auto Corners = cornerCount(NumAtoms);
  // Three bitsets, the selected indices, container overhead and APInt scratch.
  // Reserve before constructing any table, including for unlimited-work
  // callers.
  const size_t Words = Width ? (size_t(Width) - 1) / 64 + 1 : 0;
  constexpr size_t Overhead = 256;
  if (!Width || Higher.empty() || !Corners || MaxBytes < Overhead ||
      Words > (MaxBytes - Overhead) / (4 * sizeof(uint64_t)))
    return std::nullopt;
  const size_t Scratch = Overhead + Words * 4 * sizeof(uint64_t);
  if (*Corners > (MaxBytes - Scratch) / (2 * sizeof(MintermIndex) + 1) ||
      !Budget.consume(*Corners))
    return std::nullopt;

  TruthTable Active = TruthTable::zero(NumAtoms);
  for (const auto &[Key, Coeff] : Higher) {
    if (!Budget.consume(2 + Words) || Key.size() != 2 || Key[0] > Key[1] ||
        Key[1] >= *Corners || Coeff.getBitWidth() != Width || Coeff.isZero())
      return std::nullopt;
    Active.set(Key[0]);
    Active.set(Key[1]);
  }
  const auto Selected = selectedMinterms(Active);
  QuadraticProduct Result{
      llvm::APInt(Width, 0),
      {TruthTable::zero(NumAtoms), TruthTable::zero(NumAtoms)}};
  auto &Left = Result.Factors[0];
  auto &Right = Result.Factors[1];
  std::optional<MintermIndex> Anchor;
  for (MintermIndex M : Selected) {
    if (!Budget.consume(1 + Words))
      return std::nullopt;
    if (auto It = Higher.find(Monomial{M, M}); It != Higher.end()) {
      // A diagonal occurs exactly once iff both factors select this minterm.
      // It fixes the scale even when twice that scale wraps to zero.
      if (Result.Coefficient.isZero())
        Result.Coefficient = It->second;
      Left.set(M);
      Right.set(M);
    } else if (!Anchor) {
      Anchor = M;
    }
  }
  if (Result.Coefficient.isZero()) {
    // With no diagonal the factors are disjoint; every nonzero off-diagonal
    // coefficient is the scale. Pick in minterm order for stable work counts.
    for (MintermIndex M : Selected) {
      if (!Budget.consume(1 + Words))
        return std::nullopt;
      auto It = Higher.find(Monomial{Selected.front(), M});
      if (It != Higher.end()) {
        Result.Coefficient = It->second;
        break;
      }
    }
  }
  if (Result.Coefficient.isZero())
    return std::nullopt;
  if (Anchor) {
    // Every remaining minterm belongs to just one factor. Relative to one
    // anchor, an edge means the other factor and no edge means the same one.
    // Swapping the two factors is the only other possible assignment.
    for (MintermIndex M : Selected) {
      if (!Budget.consume())
        return std::nullopt;
      if (Left.at(M))
        continue;
      Monomial Edge{std::min(M, *Anchor), std::max(M, *Anchor)};
      (Higher.count(Edge) ? Right : Left).set(M);
    }
  }

  // Check the entire matrix, including absent edges and modular zeros.
  // Matching existing edges alone could accept a disconnected or incomplete
  // bipartite graph. This also rejects inconsistent scales and diagonals.
  for (size_t I = 0; I != Selected.size(); ++I) {
    const auto A = Selected[I];
    for (size_t J = I; J != Selected.size(); ++J) {
      if (!Budget.consume(1 + Words))
        return std::nullopt;
      const auto B = Selected[J];
      unsigned Count = unsigned(Left.at(A) && Right.at(B));
      if (I != J)
        Count += unsigned(Left.at(B) && Right.at(A));
      llvm::APInt Expected = Count == 2   ? Result.Coefficient.shl(1)
                             : Count == 1 ? Result.Coefficient
                                          : llvm::APInt(Width, 0);
      auto It = Higher.find(Monomial{A, B});
      if (It == Higher.end() ? !Expected.isZero() : It->second != Expected)
        return std::nullopt;
    }
  }
  return Result;
}

std::optional<CubicProduct>
recoverCubicProduct(const HigherCoefficients &Higher, unsigned NumAtoms,
                    uint32_t Width, WorkBudget &Budget, size_t MaxBytes) {
  const auto Corners = cornerCount(NumAtoms);
  const size_t Words = Width ? (size_t(Width) - 1) / 64 + 1 : 0;
  constexpr size_t Overhead = 512;
  // Four tables, membership counts, selected indices (including vector
  // capacity growth) and APInt temporaries.
  constexpr size_t CornerBytes = 8 * sizeof(MintermIndex) + 2;
  if (!Width || Higher.empty() || !Corners || MaxBytes < Overhead ||
      Words > (MaxBytes - Overhead) / (4 * sizeof(uint64_t)))
    return std::nullopt;
  size_t Scratch = Overhead + Words * 4 * sizeof(uint64_t);
  if (*Corners > (MaxBytes - Scratch) / CornerBytes ||
      !Budget.consume(*Corners))
    return std::nullopt;
  Scratch += *Corners * CornerBytes;

  TruthTable Active = TruthTable::zero(NumAtoms);
  std::optional<MintermIndex> Anchor;
  llvm::APInt Scale(Width, 0);
  for (const auto &[Key, Coeff] : Higher) {
    if (!Budget.consume(3 + Words) || Key.size() != 3 || Key[0] > Key[1] ||
        Key[1] > Key[2] || Key[2] >= *Corners || Coeff.getBitWidth() != Width ||
        Coeff.isZero())
      return std::nullopt;
    for (auto M : Key)
      Active.set(M);
    if (Key[0] == Key[2] && (!Anchor || Key[0] < *Anchor)) {
      Anchor = Key[0];
      Scale = Coeff;
    }
  }
  // The coefficient of a^3 is the scale when all three factors select a.
  // Distinguishing membership counts 0..3 requires additive order >= four.
  if (!Anchor || (Scale + Scale).isZero())
    return std::nullopt;

  auto coefficient = [&](MintermIndex A, MintermIndex B, MintermIndex C) {
    Monomial Key{A, B, C};
    llvm::sort(Key);
    auto It = Higher.find(Key);
    return It == Higher.end() ? llvm::APInt(Width, 0) : It->second;
  };
  auto scaled = [&](unsigned Count) {
    return Scale * llvm::APInt(Width, Count, false, true);
  };
  const auto Selected = selectedMinterms(Active);
  std::vector<uint8_t> Membership(*Corners, 0);
  CubicProduct Result{Scale,
                      {TruthTable::zero(NumAtoms), TruthTable::zero(NumAtoms),
                       TruthTable::zero(NumAtoms)}};
  llvm::SmallVector<MintermIndex, 3> Representatives;
  for (auto M : Selected) {
    if (!Budget.consume(1 + 4 * Words))
      return std::nullopt;
    unsigned Count = M == *Anchor ? 3 : 0;
    if (!Count) {
      // The coefficient of a^2*m counts factors selecting m.
      auto Value = coefficient(*Anchor, *Anchor, M);
      for (unsigned N = 1; N <= 3; ++N)
        if (Value == scaled(N))
          Count = N;
    }
    if (!Count)
      return std::nullopt;
    Membership[M] = Count;
    unsigned Class = 0;
    if (Count != 3) {
      // A singleton identifies its selected factor; a pair identifies its
      // omitted factor. The coefficient of a*m*n distinguishes equal and
      // unequal identities using c*(|m|*|n| - |m intersect n|).
      for (; Class < Representatives.size(); ++Class) {
        if (!Budget.consume(1 + 3 * Words))
          return std::nullopt;
        auto N = Representatives[Class];
        unsigned Other = Membership[N];
        auto Value = coefficient(*Anchor, M, N);
        unsigned Same = Count * Other - (Count == Other ? Count : 0);
        unsigned Different = Count * Other - unsigned(Count == 2 || Other == 2);
        if (Value == scaled(Same))
          break;
        if (Value != scaled(Different))
          return std::nullopt;
      }
      if (Class == Representatives.size()) {
        if (Class == 3)
          return std::nullopt;
        Representatives.push_back(M);
      }
    }
    for (unsigned F = 0; F != 3; ++F)
      if (Count == 3 || (Count == 1 ? F == Class : F != Class))
        Result.Factors[F].set(M);
  }

  // The anchor slice only proposes the factors. Expand all of them, including
  // monomials absent from the input and multiplicities vanishing modulo 2^w.
  // Bound the count map before allocating; a partial match is never a result.
  llvm::SmallVector<llvm::SmallVector<MintermIndex, 8>, 3> Choices;
  constexpr size_t EntryBytes =
      sizeof(Monomial) + sizeof(unsigned) + 6 * sizeof(void *);
  size_t ProductSize = 1;
  for (const auto &Factor : Result.Factors) {
    Choices.push_back(selectedMinterms(Factor));
    if (Choices.back().empty() ||
        ProductSize > (MaxBytes - Scratch) / EntryBytes / Choices.back().size())
      return std::nullopt;
    ProductSize *= Choices.back().size();
  }
  if (!Budget.canConsume(ProductSize))
    return std::nullopt;
  std::unordered_map<Monomial, unsigned, MonomialHash> Counts;
  forEachMonomial(Choices, Budget, [&](const Monomial &Key) {
    ++Counts[Key];
    return true;
  });
  if (Budget.exhausted())
    return std::nullopt;
  size_t Found = 0;
  for (const auto &[Key, Count] : Counts) {
    if (!Budget.consume(1 + Words))
      return std::nullopt;
    auto Expected = scaled(Count);
    auto It = Higher.find(Key);
    if (It == Higher.end() ? !Expected.isZero() : It->second != Expected)
      return std::nullopt;
    Found += It != Higher.end();
  }
  return Found == Higher.size() ? std::optional(std::move(Result))
                                : std::nullopt;
}

std::optional<SymRef> solvePolynomial(SymContext &Ctx, SymRef Body,
                                      llvm::ArrayRef<uint32_t> AtomIds,
                                      llvm::ArrayRef<SymRef> Atoms,
                                      const MBAOptions &Opts,
                                      WorkBudget &Budget) {
  // The caller's synthesis arity is a budget: tabulating a factor over this
  // many inputs is what it agreed to pay for.  Minterm indices use the same
  // 32-bit range as the truth-table container, so no narrower storage ceiling
  // silently removes the polynomial reading.
  const SolverLimits Limits = resolveLimits(Opts);
  const auto NumAtoms = static_cast<unsigned>(AtomIds.size());
  if (NumAtoms > Limits.MaxSynthesisAtoms)
    return std::nullopt;
  const std::optional<size_t> Corners = cornerCount(NumAtoms);
  if (!Corners)
    return std::nullopt;

  std::optional<llvm::SmallVector<PolyTerm, 8>> Terms =
      splitIntoTerms(Ctx, Body);
  if (!Terms)
    return std::nullopt;
  // The expansion cannot contribute a candidate without a nonlinear term.
  // Check its factors before allocating and evaluating whole minterm tables.
  if (std::none_of(Terms->begin(), Terms->end(), [](const PolyTerm &Term) {
        return Term.Factors.size() > 1;
      }))
    return std::nullopt;

  const uint32_t Width = Ctx.width(Body);
  // A product contributes monomials of exactly its own degree. Split before
  // expansion so a degree's minterm basis contains only its own inputs. A
  // linear opaque mask otherwise doubles every factor's table and multiplies
  // a cubic expansion's work by eight, although it cannot change that product.
  std::map<unsigned, llvm::SmallVector<PolyTerm, 8>> ByDegree;
  llvm::SmallVector<PolyTerm, 8> LinearTerms;
  for (auto &Term : *Terms)
    (Term.Factors.size() < 2 ? LinearTerms : ByDegree[Term.Factors.size()])
        .push_back(std::move(Term));
  auto LinearForm =
      expandOverMinterms(Ctx, LinearTerms, AtomIds, Width, Budget);
  if (!LinearForm)
    return std::nullopt;

  const size_t TermCeiling = termBudget(Ctx, Body, Opts);
  llvm::SmallVector<SymRef, 4> Linear;
  linearCandidates(Ctx, std::move(LinearForm->Linear), Atoms, TermCeiling,
                   Limits, Linear);
  SymRef Remainder = cheapestOf(Ctx, Linear);
  if (!Remainder.isValid())
    return std::nullopt;

  const BitwiseSynthesisLimits Synthesis = Limits.synthesis(TermCeiling);
  llvm::SmallVector<SymRef, 4> Parts;
  for (const auto &[Degree, DegreeTerms] : ByDegree) {
    llvm::SmallVector<uint32_t, 16> DegreeIds;
    llvm::DenseSet<uint32_t> Seen;
    for (const auto &Term : DegreeTerms)
      for (SymRef Factor : Term.Factors)
        if (Seen.insert(Factor.index()).second) {
          if (!Budget.consume(Ctx.dagSize(Factor)))
            return std::nullopt;
          Ctx.collectVars(Factor, DegreeIds);
        }
    llvm::SmallVector<SymRef, 16> DegreeAtoms;
    for (uint32_t Id : DegreeIds)
      DegreeAtoms.push_back(Ctx.varRef(Id));
    auto Form = expandOverMinterms(Ctx, DegreeTerms, DegreeIds, Width, Budget);
    if (!Form)
      return std::nullopt;
    // Exact cancellation can remove a degree altogether.
    if (Form->Higher.empty())
      continue;
    const auto &Part = Form->Higher;
    const unsigned DegreeArity = DegreeIds.size();
    const size_t MaxCandidates =
        std::max<size_t>(1, Limits.SynthesisWork / *cornerCount(DegreeArity));
    // More than one product can match, and which reads best is not the order
    // they are found in, so every match reached within the budget is ranked.
    // Ranking the product alone rather than the whole sum is the same order:
    // the remainder is the same term under every candidate.
    SymRef Best;
    SymReadability BestCost;
    auto Consider = [&](const llvm::APInt &Coeff,
                        llvm::ArrayRef<TruthTable> Match) {
      llvm::SmallVector<SymRef, 8> Factors;
      Factors.reserve(Match.size() + 1);
      Factors.push_back(Ctx.mkConst(Coeff));
      for (const TruthTable &Table : Match) {
        std::optional<SymRef> Written =
            synthesizeBitwise(Ctx, Table, DegreeAtoms, Synthesis);
        if (!Written)
          return true;
        Factors.push_back(*Written);
      }
      SymRef Candidate = Ctx.mkMul(Factors);
      const SymReadability Cost = readingScore(Ctx, Candidate);
      if (!Best.isValid() || Cost < BestCost) {
        Best = Candidate;
        BestCost = Cost;
      }
      return true;
    };
    if (Degree == 2) {
      if (auto Product = recoverQuadraticProduct(Part, DegreeArity, Width,
                                                 Budget, Opts.MaxTableBytes))
        Consider(Product->Coefficient, Product->Factors);
    } else if (Degree == 3) {
      if (auto Product = recoverCubicProduct(Part, DegreeArity, Width, Budget,
                                             Opts.MaxTableBytes))
        Consider(Product->Coefficient, Product->Factors);
      else
        forEachProductMatch(Part, Degree, DegreeArity, Width, MaxCandidates,
                            Budget, Consider);
    } else {
      forEachProductMatch(Part, Degree, DegreeArity, Width, MaxCandidates,
                          Budget, Consider);
    }
    // One unexplained degree means the whole reading is unexplained: dropping
    // it would return an expression missing a term rather than a shorter one.
    if (!Best.isValid())
      return std::nullopt;
    Parts.push_back(Best);
  }

  Parts.push_back(Remainder);
  return Ctx.mkAdd(Parts);
}

} // namespace neverd::symbolic::detail
