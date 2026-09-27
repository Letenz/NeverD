//===- SymBitwise.cpp - Truth tables and bitwise synthesis ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements synthesis of a bitwise expression from its truth table.
///
/// Exact constructions, chosen by what the caller can
/// afford and then by which answer reads better:
///
///   - Up to \c BitwiseSynthesisLimits::MaxOptimalAtoms inputs, an exhaustive
///     search.  There are only 2^(2^3) = 256 functions of three inputs, so
///     relaxing every pairwise combination until no cost improves settles the
///     shortest form of all of them at once.  The table is built once and
///     reused.  Each further input squares the count and then squares the
///     work, which is why the default stops at three and the ceiling at four.
///
///   - Above that, two exact constructions are costed against each other.  A
///     cover by prime implicants, grown by merging adjacent product terms and
///     then selected greedily, is short for functions close to a union of
///     cubes.  An exclusive-or normal form, the table inverted over the subset
///     lattice in the two-element field, is short for functions close to a
///     parity.  Running only one of them would make the quality of the answer
///     depend on which family the obfuscator happened to use: a seven-input
///     parity is sixty-four seven-literal products one way and a single
///     exclusive-or of seven atoms the other.
///     Independent parity arms can also be removed from that normal form to
///     expose a kernel of at most three inputs for the cached exact recipes.
///
/// Both of the general constructions are polynomial in the number of patterns
/// the function is true on, which is itself exponential in the arity, so each
/// is costed before it is started and the whole of synthesis reports failure
/// rather than beginning a search that would not finish.  That failure is a
/// resource answer: the caller drops the candidate form and keeps the ones it
/// could afford.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymBitwise.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::symbolic {

namespace {

constexpr size_t kCostCeiling = std::numeric_limits<size_t>::max();

/// Saturating arithmetic, so an estimate that overflows reads as "more than
/// any budget" rather than wrapping into an affordable-looking number.  Every
/// affordability test in this file is a comparison against a budget, and a
/// wrapped estimate is exactly how one of those quietly says yes to work that
/// would not finish.
size_t saturatingAdd(size_t A, size_t B) {
  return B > kCostCeiling - A ? kCostCeiling : A + B;
}

size_t saturatingMul(size_t A, size_t B) {
  if (A == 0 || B == 0)
    return 0;
  return B > kCostCeiling / A ? kCostCeiling : A * B;
}

//===----------------------------------------------------------------------===//
// Exhaustive search over the small arities
//===----------------------------------------------------------------------===//

/// How one truth table was reached during the search.
struct Recipe {
  enum Kind : uint8_t { Unreached, Zero, Ones, Atom, Not, And, Or, Xor };
  Kind K = Unreached;
  /// Atom index for \c Atom; otherwise the operand tables, packed.
  uint32_t A = 0;
  uint32_t B = 0;
};

constexpr unsigned kUnreachable = ~0u;

/// The most inputs the exhaustive search will ever tabulate.
///
/// Four inputs is 65536 functions and a relaxation over every pair of them;
/// five would be four billion recipes, which is not a longer wait but a failed
/// allocation.  Clamping a larger request rather than refusing it keeps a
/// caller that asked for more from losing the optimality it could have had.
constexpr unsigned kMaxTabulatedAtoms = 4;

/// A lower bound on the work needed to settle the optimal table.
///
/// Every function is compared with every other function at least once before
/// the relaxation can be known to have reached a fixed point.  Charging that
/// first sweep up front keeps a request with no work budget from triggering a
/// lazily-built table, especially the four-input table that is intentionally
/// expensive.
size_t optimalTableWork(unsigned NumVars) {
  assert(NumVars <= kMaxTabulatedAtoms);
  const size_t NumFunctions = size_t(1) << (size_t(1) << NumVars);
  return saturatingMul(NumFunctions, NumFunctions);
}

/// The shortest expression for every boolean function of a given small arity.
///
/// Cost counts nodes in a tree, which over-charges a shared subterm.  That
/// only affects the ranking between two forms of the same function, and at
/// these sizes the two measures agree on the winner.
class SynthTable {
public:
  explicit SynthTable(unsigned NumVars);

  const Recipe &recipe(uint32_t Packed) const { return Recipes[Packed]; }

private:
  void relax(uint32_t T, unsigned Cost, Recipe R) {
    if (Cost >= Costs[T])
      return;
    Costs[T] = Cost;
    Recipes[T] = R;
    Improved = true;
  }

  std::vector<Recipe> Recipes;
  std::vector<unsigned> Costs;
  bool Improved = false;
};

SynthTable::SynthTable(unsigned NumVars) {
  const auto Mask = static_cast<uint32_t>(TruthTable::ones(NumVars).packed());
  const uint32_t Count = Mask + 1;
  Costs.assign(Count, kUnreachable);
  Recipes.assign(Count, Recipe{});

  relax(0, 1, Recipe{Recipe::Zero, 0, 0});
  relax(Mask, 1, Recipe{Recipe::Ones, 0, 0});
  for (unsigned J = 0; J < NumVars; ++J)
    relax(static_cast<uint32_t>(atomTruthTable(J, NumVars).packed()), 1,
          Recipe{Recipe::Atom, J, 0});

  // Relax to a fixed point.  Costs only ever fall and are bounded below, so
  // this terminates; at 256 functions the handful of passes it takes is not
  // worth a priority queue.
  for (Improved = true; Improved;) {
    Improved = false;
    for (uint32_t A = 0; A < Count; ++A) {
      if (Costs[A] == kUnreachable)
        continue;
      relax(~A & Mask, Costs[A] + 1, Recipe{Recipe::Not, A, 0});
      for (uint32_t B = A + 1; B < Count; ++B) {
        if (Costs[B] == kUnreachable)
          continue;
        unsigned Cost = Costs[A] + Costs[B] + 1;
        relax(A & B, Cost, Recipe{Recipe::And, A, B});
        relax(A | B, Cost, Recipe{Recipe::Or, A, B});
        relax(A ^ B, Cost, Recipe{Recipe::Xor, A, B});
      }
    }
  }
}

const SynthTable &synthTable(unsigned NumVars) {
  assert(NumVars <= kMaxTabulatedAtoms);
  if (NumVars == kMaxTabulatedAtoms) {
    // Built only when a caller has asked for four-input optimality, because
    // building it is minutes where the smaller arities are microseconds.  A
    // caller that never raises the dial must not pay for the possibility.
    static const SynthTable Widest(kMaxTabulatedAtoms);
    return Widest;
  }
  // Built once on first use and shared thereafter: the search is cheap but not
  // free, and the simplifier calls into it in a loop.
  static const SynthTable Tables[] = {SynthTable(0), SynthTable(1),
                                      SynthTable(2), SynthTable(3)};
  return Tables[NumVars];
}

SymRef buildRecipe(SymContext &Ctx, const SynthTable &Table, uint32_t Packed,
                   llvm::ArrayRef<SymRef> Atoms, uint32_t Width,
                   llvm::DenseMap<uint32_t, SymRef> &Memo) {
  auto It = Memo.find(Packed);
  if (It != Memo.end())
    return It->second;

  const Recipe &R = Table.recipe(Packed);
  SymRef Result;
  switch (R.K) {
  case Recipe::Zero:
    Result = Ctx.mkZero(Width);
    break;
  case Recipe::Ones:
    Result = Ctx.mkOnes(Width);
    break;
  case Recipe::Atom:
    Result = Atoms[R.A];
    break;
  case Recipe::Not:
    Result = Ctx.mkNot(buildRecipe(Ctx, Table, R.A, Atoms, Width, Memo));
    break;
  case Recipe::And:
  case Recipe::Or:
  case Recipe::Xor: {
    SymRef L = buildRecipe(Ctx, Table, R.A, Atoms, Width, Memo);
    SymRef Rhs = buildRecipe(Ctx, Table, R.B, Atoms, Width, Memo);
    Result = R.K == Recipe::And  ? Ctx.mkAnd(L, Rhs)
             : R.K == Recipe::Or ? Ctx.mkOr(L, Rhs)
                                 : Ctx.mkXor(L, Rhs);
    break;
  }
  case Recipe::Unreached:
    llvm_unreachable("every function of this arity is reachable");
  }
  Memo[Packed] = Result;
  return Result;
}

/// A lower bound on the reading cost of any expression that depends on every
/// atom in \p Atoms.  It is deliberately cheap enough to compute before the
/// optimal table is requested: a cost budget that cannot hold even the atoms
/// must not trigger that table merely to learn the same answer.
size_t minimumRecipeCost(const SymContext &Ctx, llvm::ArrayRef<SymRef> Atoms) {
  size_t Cost = Atoms.size() > 1 ? 1 : 0;
  for (SymRef Atom : Atoms)
    Cost = saturatingAdd(Cost, Ctx.readabilityCost(Atom));
  return Cost;
}

//===----------------------------------------------------------------------===//
// What the atoms themselves cost
//===----------------------------------------------------------------------===//

/// The reading cost of each atom, and of its complement.
///
/// Synthesis is offered atoms that are whole expressions as often as it is
/// offered variables — a placeholder standing for a division, say — so costing
/// a candidate form by counting its literals would rank two forms by which one
/// mentions the cheap-looking atom.  Asking the context is exact and cached.
struct AtomCosts {
  llvm::SmallVector<size_t, 8> Plain;
  llvm::SmallVector<size_t, 8> Complemented;

  AtomCosts(const SymContext &Ctx, llvm::ArrayRef<SymRef> Atoms) {
    for (SymRef A : Atoms) {
      const size_t Cost = Ctx.readabilityCost(A);
      Plain.push_back(Cost);
      Complemented.push_back(saturatingAdd(Cost, 1));
    }
  }
};

/// The cost of joining \p Parts under one n-ary operator.  One part needs no
/// operator at all, which is what keeps a single-literal product from being
/// charged for an `and` that never gets built.
size_t joinCost(llvm::ArrayRef<size_t> Parts) {
  size_t Total = 0;
  for (size_t Part : Parts)
    Total = saturatingAdd(Total, Part);
  return Parts.size() <= 1 ? Total : saturatingAdd(Total, 1);
}

//===----------------------------------------------------------------------===//
// Prime implicant cover
//===----------------------------------------------------------------------===//

/// A product term: \c Care says which inputs the term constrains, \c Value
/// says what it constrains them to.  Uncared positions of \c Value are zero,
/// which is what lets two implicants be compared for equality directly.
struct Implicant {
  uint32_t Value = 0;
  uint32_t Care = 0;
};

/// The minterms an implicant covers.
TruthTable coverage(Implicant P, unsigned NumVars) {
  TruthTable Covered = TruthTable::zero(NumVars);
  for (size_t K = 0, E = Covered.entries(); K < E; ++K)
    if ((static_cast<uint32_t>(K) & P.Care) == P.Value)
      Covered.set(K);
  return Covered;
}

/// Merge product terms that differ in exactly one constrained input, dropping
/// that input from the pair, until nothing can grow any further.  Whatever
/// survives a round un-merged is prime.
///
/// Deduplication is by hash rather than by scanning what has already been
/// grown.  The scan made a round cubic in the number of implicants, which is
/// only invisible while the arity keeps that number in the dozens — exactly
/// the assumption this file no longer makes.
std::vector<Implicant> primeImplicants(const TruthTable &Table,
                                       unsigned NumVars, size_t &Work,
                                       size_t Budget) {
  const auto Full = static_cast<uint32_t>((size_t(1) << NumVars) - 1);
  std::vector<Implicant> Current;
  for (size_t K = 0, E = Table.entries(); K < E; ++K)
    if (Table.at(K))
      Current.push_back(Implicant{static_cast<uint32_t>(K), Full});

  std::vector<Implicant> Primes;
  while (!Current.empty()) {
    Work = saturatingAdd(Work, saturatingMul(Current.size(), Current.size()));
    if (Work > Budget)
      return {};

    std::vector<char> Merged(Current.size(), 0);
    std::vector<Implicant> Next;
    llvm::DenseSet<std::pair<uint32_t, uint32_t>> Seen;
    for (size_t I = 0; I < Current.size(); ++I) {
      for (size_t J = I + 1; J < Current.size(); ++J) {
        if (Current[I].Care != Current[J].Care)
          continue;
        uint32_t Diff = (Current[I].Value ^ Current[J].Value) & Current[I].Care;
        // Adjacent means differing in exactly one constrained input.
        if (Diff == 0 || (Diff & (Diff - 1)) != 0)
          continue;
        Merged[I] = Merged[J] = 1;
        Implicant Grown{Current[I].Value & ~Diff, Current[I].Care & ~Diff};
        if (Seen.insert({Grown.Value, Grown.Care}).second)
          Next.push_back(Grown);
      }
    }
    for (size_t I = 0; I < Current.size(); ++I)
      if (!Merged[I])
        Primes.push_back(Current[I]);
    Current = std::move(Next);
  }
  return Primes;
}

/// A cover of the function by product terms, and what writing it out would
/// cost.  Costing the cover before any node is built is what lets a caller
/// refuse a sixty-thousand-node form without paying to intern one first.
struct Cover {
  llvm::SmallVector<Implicant, 8> Products;
  size_t Cost = 0;
};

std::optional<Cover> sumOfProductsCover(const TruthTable &Table,
                                        const AtomCosts &Costs,
                                        size_t WorkBudget, size_t CostBudget) {
  const unsigned NumVars = Table.numVars();
  size_t Work = Table.entries();
  if (Work > WorkBudget)
    return std::nullopt;

  // An empty result here is a budget answer as much as a structural one: a
  // non-constant function always has primes, so nothing to cover with means
  // the merge rounds were stopped, and either way there is no cover to offer.
  std::vector<Implicant> Primes =
      primeImplicants(Table, NumVars, Work, WorkBudget);
  if (Primes.empty())
    return std::nullopt;

  // One coverage sweep per prime, kept rather than recomputed: the greedy loop
  // below asks each prime what it still covers on every round, and re-deriving
  // that is what turns a cover into a cubic search.
  Work = saturatingAdd(Work, saturatingMul(Primes.size(), Table.entries()));
  if (Work > WorkBudget)
    return std::nullopt;
  std::vector<TruthTable> Covers;
  Covers.reserve(Primes.size());
  for (Implicant P : Primes)
    Covers.push_back(coverage(P, NumVars));

  TruthTable Uncovered = Table;
  Cover Out;
  size_t Running = 0;
  llvm::SmallVector<size_t, 8> FactorCosts;
  while (!Uncovered.isZero()) {
    // Greedy: take whichever prime accounts for the most that is still
    // uncovered.  An exact cover would need a set-cover search, and the
    // difference is a term or two.
    size_t Best = Primes.size();
    size_t BestGain = 0;
    for (size_t I = 0; I < Primes.size(); ++I) {
      const size_t Gain = (Covers[I] & Uncovered).count();
      if (Gain > BestGain) {
        BestGain = Gain;
        Best = I;
      }
    }
    // The primes of a function cover every minterm of it, so failing to find
    // one that helps means the table and the cover have drifted apart.
    if (Best == Primes.size())
      return std::nullopt;

    Work = saturatingAdd(Work, Primes.size());
    if (Work > WorkBudget)
      return std::nullopt;

    const Implicant P = Primes[Best];
    Uncovered &= ~Covers[Best];
    Primes.erase(Primes.begin() + Best);
    Covers.erase(Covers.begin() + Best);

    FactorCosts.clear();
    for (unsigned J = 0; J < NumVars; ++J) {
      if (!(P.Care & (1u << J)))
        continue;
      FactorCosts.push_back((P.Value & (1u << J)) ? Costs.Plain[J]
                                                  : Costs.Complemented[J]);
    }
    // A product constraining nothing is the all-ones word, which reads free.
    Running =
        saturatingAdd(Running, FactorCosts.empty() ? 0 : joinCost(FactorCosts));
    Out.Products.push_back(P);
    // Abandoning a cover that has already outgrown what it may replace is the
    // point of costing as it is built rather than after.
    if (Running > CostBudget)
      return std::nullopt;
  }

  Out.Cost = Out.Products.size() <= 1 ? Running : saturatingAdd(Running, 1);
  if (Out.Cost > CostBudget)
    return std::nullopt;
  return Out;
}

SymRef buildCover(SymContext &Ctx, const Cover &C, llvm::ArrayRef<SymRef> Atoms,
                  uint32_t Width) {
  const auto NumVars = static_cast<unsigned>(Atoms.size());
  llvm::SmallVector<SymRef, 8> Products;
  for (Implicant P : C.Products) {
    llvm::SmallVector<SymRef, 8> Factors;
    for (unsigned J = 0; J < NumVars; ++J) {
      if (!(P.Care & (1u << J)))
        continue;
      Factors.push_back((P.Value & (1u << J)) ? Atoms[J] : Ctx.mkNot(Atoms[J]));
    }
    Products.push_back(Factors.empty() ? Ctx.mkOnes(Width)
                                       : Ctx.mkAnd(Factors));
  }
  if (Products.empty())
    return Ctx.mkZero(Width);
  return Ctx.mkOr(Products);
}

//===----------------------------------------------------------------------===//
// Exclusive-or normal form
//===----------------------------------------------------------------------===//

/// The conjunctions whose exclusive-or is \p Table, each named by the set of
/// inputs it conjoins.
///
/// Every boolean function is one such exclusive-or and only one, and the
/// coefficients are the table inverted over the subset lattice in the
/// two-element field — the same inversion the linear reading performs over the
/// integers to find its conjunction coefficients.  Subtracting one dimension
/// at a time is what makes it cost t * 2^t rather than the 3^t a direct walk
/// of submasks would.
///
/// The empty conjunction stands for the all-ones word.
llvm::SmallVector<uint32_t, 16> exclusiveOrTerms(const TruthTable &Table) {
  const unsigned NumVars = Table.numVars();
  const size_t Entries = Table.entries();
  TruthTable Coefficients = Table;
  for (unsigned J = 0; J < NumVars; ++J) {
    const size_t Bit = size_t(1) << J;
    for (size_t K = 0; K < Entries; ++K)
      if (K & Bit)
        Coefficients.setValue(K, Coefficients.at(K) ^ Coefficients.at(K ^ Bit));
  }

  llvm::SmallVector<uint32_t, 16> Terms;
  for (size_t K = 0; K < Entries; ++K)
    if (Coefficients.at(K))
      Terms.push_back(static_cast<uint32_t>(K));
  return Terms;
}

size_t exclusiveOrCost(llvm::ArrayRef<uint32_t> Terms, const AtomCosts &Costs,
                       unsigned NumVars, size_t Budget) {
  size_t Running = 0;
  llvm::SmallVector<size_t, 8> FactorCosts;
  for (uint32_t Term : Terms) {
    FactorCosts.clear();
    for (unsigned J = 0; J < NumVars; ++J)
      if (Term & (1u << J))
        FactorCosts.push_back(Costs.Plain[J]);
    // The empty conjunction is the all-ones literal, which reads free.
    Running =
        saturatingAdd(Running, FactorCosts.empty() ? 0 : joinCost(FactorCosts));
    if (Running > Budget)
      return kCostCeiling;
  }
  return Terms.size() <= 1 ? Running : saturatingAdd(Running, 1);
}

SymRef buildExclusiveOr(SymContext &Ctx, llvm::ArrayRef<uint32_t> Terms,
                        llvm::ArrayRef<SymRef> Atoms, uint32_t Width) {
  const auto NumVars = static_cast<unsigned>(Atoms.size());
  llvm::SmallVector<SymRef, 16> Parts;
  for (uint32_t Term : Terms) {
    llvm::SmallVector<SymRef, 8> Factors;
    for (unsigned J = 0; J < NumVars; ++J)
      if (Term & (1u << J))
        Factors.push_back(Atoms[J]);
    Parts.push_back(Factors.empty() ? Ctx.mkOnes(Width) : Ctx.mkAnd(Factors));
  }
  if (Parts.empty())
    return Ctx.mkZero(Width);
  return Ctx.mkXor(Parts);
}

/// Cost a cached small recipe before interning it. This upper bound keeps the
/// binary recipe's joins even when the builders would flatten or cancel them.
size_t recipeCost(const SynthTable &Table, uint32_t Packed,
                  llvm::ArrayRef<size_t> Atoms,
                  llvm::DenseMap<uint32_t, size_t> &Memo) {
  auto It = Memo.find(Packed);
  if (It != Memo.end())
    return It->second;
  const Recipe &R = Table.recipe(Packed);
  size_t Cost;
  switch (R.K) {
  case Recipe::Zero:
    Cost = 1;
    break;
  case Recipe::Ones:
    Cost = 0;
    break;
  case Recipe::Atom:
    Cost = Atoms[R.A];
    break;
  case Recipe::Not:
    Cost = saturatingAdd(1, recipeCost(Table, R.A, Atoms, Memo));
    break;
  case Recipe::And:
  case Recipe::Or:
  case Recipe::Xor:
    Cost = saturatingAdd(1, saturatingAdd(recipeCost(Table, R.A, Atoms, Memo),
                                          recipeCost(Table, R.B, Atoms, Memo)));
    break;
  case Recipe::Unreached:
    llvm_unreachable("every function of this arity is reachable");
  }
  Memo[Packed] = Cost;
  return Cost;
}

struct ParityKernel {
  llvm::SmallVector<unsigned, 3> Inputs;
  uint32_t Peeled = 0;
  uint32_t Packed = 0;
};

/// A variable appearing only as a singleton ANF term is an independent XOR
/// arm. Removing all such arms can leave a small function for which the exact
/// recipe is already available. Never enlarge the exhaustive search for this
/// candidate, even when a caller explicitly permits a larger table elsewhere.
std::optional<ParityKernel> parityKernel(llvm::ArrayRef<uint32_t> Terms,
                                         const AtomCosts &Costs,
                                         unsigned NumVars, size_t ANFWork,
                                         size_t BestCost,
                                         const BitwiseSynthesisLimits &Limits) {
  const unsigned Ceiling = std::min(Limits.MaxOptimalAtoms, 3u);
  if (Ceiling < 2 || ANFWork > Limits.MaxWork ||
      Terms.size() > Limits.MaxWork - ANFWork)
    return std::nullopt;
  size_t Work = ANFWork + Terms.size();
  auto charge = [&](size_t Amount) {
    if (Amount > Limits.MaxWork - Work)
      return false;
    Work += Amount;
    return true;
  };
  uint32_t Nonlinear = 0, Singletons = 0;
  for (uint32_t Term : Terms) {
    if (std::has_single_bit(Term)) {
      Singletons |= Term;
    } else {
      Nonlinear |= Term;
      if (std::popcount(Nonlinear) > Ceiling)
        return std::nullopt;
    }
  }
  ParityKernel Out;
  Out.Peeled = Singletons & ~Nonlinear;
  if (!Out.Peeled || !Nonlinear || !charge(NumVars))
    return std::nullopt;

  // Every supported atom must still be read. Reject unaffordable candidates
  // before requesting the cached recipe or constructing any expression.
  const size_t Limit = std::min(BestCost, Limits.MaxCost);
  if (joinCost(Costs.Plain) >= Limit)
    return std::nullopt;
  llvm::SmallVector<size_t, 3> KernelCosts;
  size_t PeeledCost = 0;
  for (unsigned I = 0; I < NumVars; ++I) {
    if (Nonlinear & (1u << I)) {
      Out.Inputs.push_back(I);
      KernelCosts.push_back(Costs.Plain[I]);
    } else if (Out.Peeled & (1u << I)) {
      PeeledCost = saturatingAdd(PeeledCost, Costs.Plain[I]);
    }
  }
  const auto Size = static_cast<unsigned>(Out.Inputs.size());
  const unsigned Entries = 1u << Size;
  // The ANF scan and tiny projection share the original construction's work
  // allowance. In particular, the kernel does not get a fresh optimal budget.
  if (!charge(saturatingMul(Entries, saturatingAdd(Terms.size(), Size))) ||
      !charge(optimalTableWork(Size)))
    return std::nullopt;
  for (unsigned K = 0; K < Entries; ++K) {
    uint32_t Original = 0;
    for (unsigned I = 0; I < Size; ++I)
      if (K & (1u << I))
        Original |= 1u << Out.Inputs[I];
    bool Value = false;
    for (uint32_t Term : Terms)
      if ((Original & Term) == Term)
        Value = !Value;
    if (Value)
      Out.Packed |= 1u << K;
  }
  // There are at most 256 recipes. Bound their cost memo and construction
  // together; recursion follows only these fixed small recipes, never a DAG
  // supplied by the caller.
  if (!charge(2 * (size_t(1) << Entries)))
    return std::nullopt;
  llvm::DenseMap<uint32_t, size_t> Memo;
  const size_t Cost = saturatingAdd(
      1, saturatingAdd(PeeledCost, recipeCost(synthTable(Size), Out.Packed,
                                              KernelCosts, Memo)));
  if (Cost > Limits.MaxCost || Cost >= BestCost)
    return std::nullopt;
  return Out;
}

SymRef buildParityKernel(SymContext &Ctx, const ParityKernel &Kernel,
                         llvm::ArrayRef<SymRef> Atoms, uint32_t Width) {
  llvm::SmallVector<SymRef, 3> Core;
  for (unsigned I : Kernel.Inputs)
    Core.push_back(Atoms[I]);
  llvm::DenseMap<uint32_t, SymRef> Memo;
  llvm::SmallVector<SymRef, 8> Parts;
  Parts.push_back(buildRecipe(Ctx, synthTable(Core.size()), Kernel.Packed, Core,
                              Width, Memo));
  for (unsigned I = 0; I < Atoms.size(); ++I)
    if (Kernel.Peeled & (1u << I))
      Parts.push_back(Atoms[I]);
  return Ctx.mkXor(Parts);
}

//===----------------------------------------------------------------------===//
// Dropping the inputs the function ignores
//===----------------------------------------------------------------------===//

/// Restrict \p Table to the inputs listed in \p Kept, renumbering them densely.
/// Sound because the caller has already established that the dropped inputs
/// cannot change the result.
TruthTable projectTruthTable(const TruthTable &Table,
                             llvm::ArrayRef<unsigned> Kept) {
  const auto NumKept = static_cast<unsigned>(Kept.size());
  if (NumKept == Table.numVars())
    return Table;

  TruthTable Out = TruthTable::zero(NumKept);
  for (size_t P = 0, E = Out.entries(); P < E; ++P) {
    size_t Original = 0;
    for (unsigned I = 0; I < NumKept; ++I)
      if (P & (size_t(1) << I))
        Original |= size_t(1) << Kept[I];
    if (Table.at(Original))
      Out.set(P);
  }
  return Out;
}

} // namespace

TruthTable atomTruthTable(unsigned Index, unsigned NumVars) {
  TruthTable Out = TruthTable::zero(NumVars);
  for (size_t K = 0, E = Out.entries(); K < E; ++K)
    if ((K >> Index) & 1)
      Out.set(K);
  return Out;
}

uint32_t truthTableSupport(const TruthTable &Table) {
  const unsigned NumVars = Table.numVars();
  const size_t Entries = Table.entries();
  uint32_t Support = 0;
  for (unsigned J = 0; J < NumVars; ++J) {
    const size_t Bit = size_t(1) << J;
    for (size_t K = 0; K < Entries; ++K) {
      if (K & Bit)
        continue;
      if (Table.at(K) != Table.at(K | Bit)) {
        Support |= static_cast<uint32_t>(Bit);
        break;
      }
    }
  }
  return Support;
}

std::optional<SymRef> synthesizeBitwise(SymContext &Ctx,
                                        const TruthTable &Table,
                                        llvm::ArrayRef<SymRef> Atoms,
                                        const BitwiseSynthesisLimits &Limits) {
  assert(!Atoms.empty() && "synthesis needs at least one atom for its width");
  assert(Atoms.size() == Table.numVars() &&
         "the table's arity has to be the number of atoms it is written over");

  const uint32_t Width = Ctx.width(Atoms[0]);
  if (Table.isZero())
    return Ctx.mkZero(Width);
  if (Table.isOnes())
    return Ctx.mkOnes(Width);

  // Neither constant, so at least one input matters.
  const uint32_t Support = truthTableSupport(Table);
  llvm::SmallVector<unsigned, 8> Kept;
  llvm::SmallVector<SymRef, 8> KeptAtoms;
  for (unsigned J = 0, E = Table.numVars(); J < E; ++J) {
    if (!(Support & (1u << J)))
      continue;
    Kept.push_back(J);
    KeptAtoms.push_back(Atoms[J]);
  }

  const TruthTable Projected = projectTruthTable(Table, Kept);
  const auto NumVars = static_cast<unsigned>(Kept.size());
  const unsigned OptimalCeiling =
      std::min(Limits.MaxOptimalAtoms, kMaxTabulatedAtoms);
  if (NumVars <= OptimalCeiling) {
    if (optimalTableWork(NumVars) > Limits.MaxWork)
      return std::nullopt;
    if (minimumRecipeCost(Ctx, KeptAtoms) > Limits.MaxCost)
      return std::nullopt;

    const SynthTable &Table = synthTable(NumVars);
    const auto Packed = static_cast<uint32_t>(Projected.packed());
    llvm::DenseMap<uint32_t, SymRef> Memo;
    SymRef Result = buildRecipe(Ctx, Table, Packed, KeptAtoms, Width, Memo);
    if (Ctx.readabilityCost(Result) > Limits.MaxCost)
      return std::nullopt;
    return Result;
  }

  const AtomCosts Costs(Ctx, KeptAtoms);

  // Both constructions are exact, so this ranks two spellings of one function
  // rather than choosing what the answer means.  A tie goes to the cover,
  // which is the form a reader is more likely to recognise.
  std::optional<Cover> Sum =
      sumOfProductsCover(Projected, Costs, Limits.MaxWork, Limits.MaxCost);

  size_t ExclusiveCost = kCostCeiling;
  llvm::SmallVector<uint32_t, 16> Terms;
  if (saturatingMul(Projected.entries(), NumVars) <= Limits.MaxWork) {
    Terms = exclusiveOrTerms(Projected);
    ExclusiveCost = exclusiveOrCost(Terms, Costs, NumVars, Limits.MaxCost);
  }

  const size_t SumCost = Sum ? Sum->Cost : kCostCeiling;
  std::optional<ParityKernel> Kernel;
  if (!Terms.empty())
    Kernel = parityKernel(Terms, Costs, NumVars,
                          saturatingMul(Projected.entries(), NumVars),
                          std::min(SumCost, ExclusiveCost), Limits);
  SymRef Best;
  if (SumCost <= ExclusiveCost && SumCost != kCostCeiling)
    Best = buildCover(Ctx, *Sum, KeptAtoms, Width);
  else if (ExclusiveCost != kCostCeiling && ExclusiveCost <= Limits.MaxCost)
    Best = buildExclusiveOr(Ctx, Terms, KeptAtoms, Width);
  if (Kernel) {
    SymRef Candidate = buildParityKernel(Ctx, *Kernel, KeptAtoms, Width);
    // Opaque or repeated atoms may make the established form collapse during
    // construction. Keep it on an actual-cost tie as well as an estimated one.
    if (!Best.isValid() ||
        Ctx.readabilityCost(Candidate) < Ctx.readabilityCost(Best))
      Best = Candidate;
  }
  return Best.isValid() ? std::optional<SymRef>(Best) : std::nullopt;
}

} // namespace neverd::symbolic
