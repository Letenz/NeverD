//===- SymMBAMeasure.cpp - Reading and rewriting minterm weights ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the corner measurement and the forms the weights it reads are
/// written back out in.
///
/// Setting every input to all-zeros or all-ones puts every bit position into
/// the same pattern k, which leaves the minterm `M_k` at all-ones and every
/// other at zero, so the expression evaluates to `-w_k`.  One evaluation per
/// pattern therefore reads off every weight, and the size the expression was
/// written at has nothing to do with it.
///
/// Four ways of writing the weights back out are tried, because none of them
/// is shortest for every function:
///
///   - *Constant*, when the weights agree.
///   - *Grouped*, `sum over distinct weights v of v * B_v` where `B_v` selects
///     the patterns carrying that weight.  This is what turns
///     `(x | y) - (x & y)` back into `x ^ y`.
///   - *Conjunction basis*, the weights inverted over the subset lattice.
///     This is what turns `(x ^ y) + 2 * (x & y)` back into `x + y`, which the
///     grouped form cannot: it would return the input.
///   - *Nested*, weighted differences over cumulative selectors. Overlapping
///     selectors can spell the same weights more cheaply than disjoint ones.
///
/// They are offered in that order and the caller keeps the cheapest, so the
/// order they are appended in decides ties.
///
/// A later small-region search subtracts one affine atom and synthesizes a
/// two-valued residual. It spends the remaining shared budget only after the
/// established linear and polynomial readings have been proved.
///
//===----------------------------------------------------------------------===//

#include "SymMBADetail.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <vector>

namespace neverd::symbolic::detail {

namespace {

/// Invert a subset-sum over the lattice of input patterns, in place.
///
/// On entry `C[K]` holds the weight of pattern K; on exit it holds the
/// coefficient of the conjunction K, meaning the value that makes the sum of
/// `C[S]` over every subset S of K reproduce the weight that was there.
/// Subtracting one dimension at a time is what makes this cost t * 2^t rather
/// than the 3^t a direct enumeration of submasks would.
void invertOverSubsets(std::vector<llvm::APInt> &C, unsigned NumAtoms) {
  for (unsigned J = 0; J < NumAtoms; ++J) {
    const size_t Bit = size_t(1) << J;
    for (size_t K = 0; K < C.size(); ++K)
      if (K & Bit)
        C[K] -= C[K ^ Bit];
  }
}

/// Order weights by value.  They all share a width, so an unsigned compare is
/// total.
struct APIntLess {
  bool operator()(const llvm::APInt &A, const llvm::APInt &B) const {
    return A.ult(B);
  }
};

/// Whether the weights already describe a sum of independently scaled inputs.
///
/// A zero constant weight and W[K] = W[K without one bit] + W[that bit]
/// characterize this form modulo the word width, including negative and wide
/// coefficients. The cumulative selectors otherwise spend a combinatorial
/// search on threshold functions for an ordinary weighted variable sum.
bool isIndependentWeightedSum(llvm::ArrayRef<llvm::APInt> Weights) {
  if (!Weights[0].isZero())
    return false;
  for (size_t K = 1; K < Weights.size(); ++K) {
    const size_t Rest = K & (K - 1);
    if (Rest != 0 && Weights[K] != Weights[Rest] + Weights[K ^ Rest])
      return false;
  }
  return true;
}

/// `sum over distinct weights v of v * B_v`, with `B_v` selecting the patterns
/// carrying that weight.  Exact because the minterms of one weight are
/// disjoint, so their union is a bitwise function like any other.
std::optional<SymRef> groupedForm(SymContext &Ctx,
                                  llvm::ArrayRef<llvm::APInt> Weights,
                                  llvm::ArrayRef<SymRef> Atoms,
                                  size_t TermBudget, const SolverLimits &Limits,
                                  llvm::SmallVectorImpl<SymRef> &Nested) {
  const auto NumAtoms = static_cast<unsigned>(Atoms.size());
  if (NumAtoms > Limits.MaxSynthesisAtoms)
    return std::nullopt;

  // Each group carries a table of one bit per corner, so how many groups may
  // be held at once is not only the term budget: at twenty inputs a table is
  // a hundred kilobytes, and a measurement whose every corner has a weight of
  // its own would otherwise build a million of them before anybody counted.
  const size_t Entries = Weights.size();
  const size_t Holdable = std::max<size_t>(1, Limits.SynthesisWork / Entries);
  const size_t GroupBudget = std::min(TermBudget, Holdable);

  // Ordered rather than hashed, so the terms come out the same way on every
  // run and a difference in output is a real change instead of a reshuffle.
  std::map<llvm::APInt, TruthTable, APIntLess> Groups;
  for (size_t K = 0; K < Entries; ++K) {
    if (Weights[K].isZero())
      continue;
    auto It = Groups.find(Weights[K]);
    if (It == Groups.end()) {
      // Stopping at the budget rather than after it is the difference between
      // rejecting a form and allocating one first.
      if (Groups.size() >= GroupBudget)
        return std::nullopt;
      It = Groups.emplace(Weights[K], TruthTable::zero(NumAtoms)).first;
    }
    It->second.set(K);
  }
  if (Groups.empty())
    return Ctx.mkZero(Ctx.width(Atoms[0]));

  const BitwiseSynthesisLimits Synthesis = Limits.synthesis(TermBudget);
  // Only the new cumulative search is skipped for independent weighted sums;
  // the grouped and conjunction candidates remain available, since this test
  // establishes a shape rather than a minimum expression cost.
  if (Groups.size() > 1 && Groups.size() <= Holdable / 2 &&
      !isIndependentWeightedSum(Weights)) {
    TruthTable Nonzero = TruthTable::zero(NumAtoms);
    for (const auto &[Value, Table] : Groups)
      Nonzero |= Table;

    // For weights v1,...,vn in any fixed order, let Si select groups i..n.
    // Then sum_i (vi - v(i-1)) * Si, with v0=0, has weight vj on group j.
    // This telescopes modulo the word width, including negative coefficients.
    // Both orders are useful because complements can make one selector cheap.
    auto appendNested = [&](const auto &Ordered) {
      TruthTable Remaining = Nonzero;
      llvm::APInt Previous(Ctx.width(Atoms[0]), 0);
      llvm::SmallVector<SymRef, 8> Terms;
      for (const auto &[Value, Table] : Ordered) {
        std::optional<SymRef> Selector =
            synthesizeBitwise(Ctx, Remaining, Atoms, Synthesis);
        if (!Selector)
          return;
        Terms.push_back(Ctx.mkMul(Ctx.mkConst(Value - Previous), *Selector));
        Previous = Value;
        Remaining ^= Table;
      }
      Nested.push_back(Ctx.mkAdd(Terms));
    };
    appendNested(Groups);
    appendNested(llvm::reverse(Groups));
  }

  llvm::SmallVector<SymRef, 8> Terms;
  for (const auto &[Value, Table] : Groups) {
    // Synthesis declines when no spelling of the selector fits the budget.
    // That is one candidate form lost, not a failure: the conjunction basis
    // below is offered from the same weights and has no table to build.
    std::optional<SymRef> Selector =
        synthesizeBitwise(Ctx, Table, Atoms, Synthesis);
    if (!Selector)
      return std::nullopt;
    Terms.push_back(Ctx.mkMul(Ctx.mkConst(Value), *Selector));
  }
  return Ctx.mkAdd(Terms);
}

/// `sum over subsets S of c_S * AND(inputs in S)`, the conjunction basis.
///
/// The empty conjunction is the all-ones word, so its coefficient contributes
/// the constant term with the sign turned around.
std::optional<SymRef> conjunctionForm(SymContext &Ctx,
                                      std::vector<llvm::APInt> Coefficients,
                                      llvm::ArrayRef<SymRef> Atoms,
                                      size_t TermBudget,
                                      const SolverLimits &Limits) {
  const auto NumAtoms = static_cast<unsigned>(Atoms.size());
  const uint32_t Width = Ctx.width(Atoms[0]);

  // The inversion touches every entry once per input, so it costs t * 2^t wide
  // subtractions before anything can be said about how many terms come out of
  // it.  That was invisible while the arity dial was a fixed sixteen and it is
  // tens of millions of operations at the arities the dial now reaches, which
  // makes it the one step of the linear reading large enough to be worth
  // costing.  Dividing rather than multiplying, because the product is exactly
  // the quantity that would not fit.
  if (NumAtoms != 0 && Coefficients.size() > Limits.SynthesisWork / NumAtoms)
    return std::nullopt;

  invertOverSubsets(Coefficients, NumAtoms);

  size_t NumTerms = 0;
  for (const llvm::APInt &C : Coefficients)
    NumTerms += !C.isZero();
  if (NumTerms > TermBudget)
    return std::nullopt;

  llvm::SmallVector<SymRef, 16> Terms;
  for (size_t K = 0; K < Coefficients.size(); ++K) {
    const llvm::APInt &C = Coefficients[K];
    if (C.isZero())
      continue;
    if (K == 0) {
      Terms.push_back(Ctx.mkConst(-C));
      continue;
    }
    llvm::SmallVector<SymRef, 16> Factors;
    for (unsigned J = 0; J < NumAtoms; ++J)
      if (K & (size_t(1) << J))
        Factors.push_back(Atoms[J]);
    Terms.push_back(Ctx.mkMul(Ctx.mkConst(C), Ctx.mkAnd(Factors)));
  }
  if (Terms.empty())
    return Ctx.mkZero(Width);
  return Ctx.mkAdd(Terms);
}

llvm::APInt randomWord(std::mt19937_64 &Rng, uint32_t Width) {
  llvm::SmallVector<uint64_t, 4> Words((Width + 63) / 64);
  for (uint64_t &Word : Words)
    Word = Rng();
  return llvm::APInt(Width, Words);
}

/// The largest t with 2^t entries or fewer.
unsigned atomsFittingEntries(size_t Entries) {
  unsigned Atoms = 0;
  while (Atoms < kMaxTruthTableAtoms && (size_t(1) << (Atoms + 1)) <= Entries)
    ++Atoms;
  return Atoms;
}

} // namespace

std::optional<size_t> cornerCount(size_t NumAtoms) {
  if (NumAtoms > kMaxTruthTableAtoms)
    return std::nullopt;
  return patternCount(static_cast<unsigned>(NumAtoms));
}

SolverLimits resolveLimits(const MBAOptions &Opts) {
  // A corner table holds one weight per corner.  Charging each at the inline
  // size of a weight is an order of magnitude rather than an allocator's
  // ledger — a word wider than sixty-four bits adds a heap allocation on top —
  // and an order of magnitude is what the dial is for: it decides whether a
  // measurement is attempted at all, not how the memory is laid out.
  const unsigned Affordable =
      atomsFittingEntries(Opts.MaxTableBytes / sizeof(llvm::APInt));

  SolverLimits Out;
  Out.MaxAtoms =
      std::min(Opts.MaxAtoms == MBAOptions::Unlimited ? kMaxTruthTableAtoms
                                                      : Opts.MaxAtoms,
               Affordable);
  // Never wider than the measurement that produced the weights: a truth table
  // over more inputs than were measured describes nothing the solver holds.
  Out.MaxSynthesisAtoms = std::min(
      Opts.MaxSynthesisAtoms == MBAOptions::Unlimited ? kMaxTruthTableAtoms
                                                      : Opts.MaxSynthesisAtoms,
      Out.MaxAtoms);
  Out.MaxOptimalSynthesisAtoms = Opts.MaxOptimalSynthesisAtoms;

  // A truth table is a bit an entry, so the same byte ceiling buys eight times
  // what a corner table does.  Synthesis charges itself a unit per bit it
  // touches and per bit it keeps, so this one number bounds both its time and
  // what it holds while spending it.
  constexpr size_t Ceiling = std::numeric_limits<size_t>::max();
  const size_t ByBytes =
      Opts.MaxTableBytes > Ceiling / 8 ? Ceiling : Opts.MaxTableBytes * 8;
  Out.SynthesisWork = std::min(Opts.MaxWork, ByBytes);
  return Out;
}

std::vector<llvm::APInt> measure(const SymContext &Ctx, SymRef Body,
                                 llvm::ArrayRef<uint32_t> Atoms) {
  const uint32_t Width = Ctx.width(Body);
  const auto NumAtoms = static_cast<unsigned>(Atoms.size());
  const std::optional<size_t> Corners = cornerCount(NumAtoms);
  assert(Corners && "corner table does not fit the host address space");

  SymEvalPlan Plan(Ctx, Body);
  std::vector<llvm::APInt> Weights(*Corners);

  if (Plan.fitsU64()) {
    const uint64_t Ones =
        Width == 64 ? ~uint64_t(0) : (uint64_t(1) << Width) - 1;
    std::vector<uint64_t> Assignment(Ctx.numVars(), 0);
    for (size_t K = 0; K < *Corners; ++K) {
      // Consecutive Gray corners change one input. Store each reading at its
      // ordinary binary index so coefficient and selector consumers keep the
      // same table convention without rewriting every atom at every corner.
      if (K != 0)
        Assignment[Atoms[std::countr_zero(K)]] ^= Ones;
      const size_t Pattern = K ^ (K >> 1);
      // The corner value is the negated weight, so negating is what turns a
      // reading into a weight.
      Weights[Pattern] =
          -llvm::APInt(Width, Plan.evalU64(Assignment),
                       /*isSigned=*/false, /*implicitTrunc=*/true);
    }
    return Weights;
  }

  const llvm::APInt Zero(Width, 0);
  std::vector<llvm::APInt> Assignment(Ctx.numVars(), Zero);
  for (size_t K = 0; K < *Corners; ++K) {
    if (K != 0)
      Assignment[Atoms[std::countr_zero(K)]].flipAllBits();
    Weights[K ^ (K >> 1)] = -Plan.eval(Assignment);
  }
  return Weights;
}

size_t readingCost(const SymContext &Ctx, SymRef R) {
  return Ctx.readabilityCost(R);
}

SymReadability readingScore(const SymContext &Ctx, SymRef R) {
  return Ctx.readability(R);
}

bool doesNotGrow(const SymContext &Ctx, SymRef Candidate, SymRef Input) {
  const SymReadability Score = readingScore(Ctx, Candidate);
  // Saturation cannot establish that two actual printed trees have equal size.
  return Score.Nodes != std::numeric_limits<size_t>::max() &&
         Score <= readingScore(Ctx, Input);
}

bool agreeOnSamples(const SymContext &Ctx, SymRef A, SymRef B,
                    unsigned Samples) {
  SymEvalPlan PlanA(Ctx, A);
  SymEvalPlan PlanB(Ctx, B);

  std::vector<llvm::APInt> Assignment;
  Assignment.reserve(Ctx.numVars());
  for (size_t I = 0; I < Ctx.numVars(); ++I)
    Assignment.emplace_back(Ctx.varInfo(uint32_t(I)).Width, 0);

  std::mt19937_64 Rng(0x9E3779B97F4A7C15ull);
  for (unsigned S = 0; S < Samples; ++S) {
    for (size_t I = 0; I < Assignment.size(); ++I) {
      uint32_t W = Ctx.varInfo(uint32_t(I)).Width;
      // The first few rounds pin every input to a corner of the space, where a
      // mistake in the corner arithmetic itself would show; the rest are
      // ordinary values, where a mistake in the algebra would.
      switch (S) {
      case 0:
        Assignment[I] = llvm::APInt(W, 0);
        break;
      case 1:
        Assignment[I] = llvm::APInt::getAllOnes(W);
        break;
      case 2:
        Assignment[I] = llvm::APInt(W, 1);
        break;
      default:
        Assignment[I] = randomWord(Rng, W);
        break;
      }
    }
    if (PlanA.eval(Assignment) != PlanB.eval(Assignment))
      return false;
  }
  return true;
}

size_t termBudget(const SymContext &Ctx, SymRef E, const MBAOptions &Opts) {
  return Opts.AllowGrowth ? std::numeric_limits<size_t>::max()
                          : readingCost(Ctx, E);
}

void linearCandidates(SymContext &Ctx, std::vector<llvm::APInt> Weights,
                      llvm::ArrayRef<SymRef> Atoms, size_t TermBudget,
                      const SolverLimits &Limits,
                      llvm::SmallVectorImpl<SymRef> &Out) {
  auto append = [&](SymRef Form) {
    if (!llvm::is_contained(Out, Form))
      Out.push_back(Form);
  };
  if (llvm::all_of(Weights,
                   [&](const llvm::APInt &W) { return W == Weights[0]; }))
    append(Ctx.mkConst(-Weights[0]));
  llvm::SmallVector<SymRef, 2> Nested;
  if (std::optional<SymRef> Grouped =
          groupedForm(Ctx, Weights, Atoms, TermBudget, Limits, Nested))
    append(*Grouped);
  // Handing the weights over rather than copying: at the widths and arity this
  // reaches, a copy would be tens of thousands of allocations.
  if (std::optional<SymRef> Conjunctions =
          conjunctionForm(Ctx, std::move(Weights), Atoms, TermBudget, Limits))
    append(*Conjunctions);
  // Preserve the established tie order; the new forms must improve the cost
  // to displace a grouped or conjunction-basis answer.
  for (SymRef Form : Nested)
    append(Form);
}

void affineResidualCandidates(SymContext &Ctx,
                              llvm::ArrayRef<llvm::APInt> Weights,
                              llvm::ArrayRef<SymRef> Atoms, size_t TermBudget,
                              const SolverLimits &Limits, WorkBudget &Budget,
                              llvm::SmallVectorImpl<SymRef> &Out,
                              bool *MayHaveBooleanPair) {
  if (MayHaveBooleanPair)
    *MayHaveBooleanPair = false;
  const unsigned Count = static_cast<unsigned>(Atoms.size());
  // This search reuses only the existing small exact Boolean recipes. A
  // larger caller ceiling must not trigger the four-input optimal table here.
  if (Count < 2 || Count > std::min({3u, Limits.MaxOptimalSynthesisAtoms,
                                     Limits.MaxSynthesisAtoms}))
    return;
  if (TermBudget < 3)
    return;
  assert(Weights.size() == (size_t(1) << Count));
  if (!Budget.consume(Weights.size()))
    return;
  llvm::SmallVector<llvm::APInt, 4> Values;
  for (const llvm::APInt &Value : Weights) {
    if (llvm::is_contained(Values, Value))
      continue;
    if (Values.size() == 4)
      return;
    Values.push_back(Value);
  }
  // At most two values already have the offset/selector readings above. A
  // shifted two-valued residual can cover at most four original values.
  if (Values.size() < 3)
    return;

  if (!Budget.consume(Count * Weights.size()))
    return;
  unsigned Support = 0;
  for (unsigned Axis = 0; Axis < Count; ++Axis) {
    const size_t Bit = size_t(1) << Axis;
    for (size_t K = 0; K < Weights.size(); ++K)
      if (!(K & Bit) && Weights[K] != Weights[K | Bit]) {
        ++Support;
        break;
      }
  }
  // Every essential input needs a leaf and connecting the leaves requires
  // at least Support - 1 printed binary operations. The three distinct
  // values above guarantee more than one essential input.
  if (TermBudget < 2 * Support - 1)
    return;

  // Reuse this charged classification in the later pair reading. Most
  // regions cannot have that form and should not scan their weights twice.
  if (MayHaveBooleanPair)
    *MayHaveBooleanPair = Support == 3 && Values.size() == 4;

  llvm::DenseMap<uint32_t, SymRef> Synthesized;

  for (unsigned Axis = 0; Axis < Count; ++Axis) {
    if (!Budget.consume(Weights.size()))
      return;
    llvm::SmallVector<llvm::APInt, 2> Low, High;
    bool TooMany = false;
    for (size_t K = 0; K < Weights.size(); ++K) {
      auto &Half = (K & (size_t(1) << Axis)) ? High : Low;
      if (llvm::is_contained(Half, Weights[K]))
        continue;
      if (Half.size() == 2) {
        TooMany = true;
        break;
      }
      Half.push_back(Weights[K]);
    }
    if (TooMany)
      continue;

    // Subtracting c*X leaves the low half unchanged and translates the high
    // half by -c. If the low half has two values, any chosen high value must
    // align with one of them. Otherwise one of the two high values must align
    // with the single low value. Thus at most two coefficients suffice; edge
    // differences alone miss valid translations. No inverse is needed, even
    // for coefficients that are not units modulo the word width.
    llvm::SmallVector<llvm::APInt, 2> Coefficients;
    if (Low.size() == 2) {
      Coefficients.push_back(High[0] - Low[0]);
      Coefficients.push_back(High[0] - Low[1]);
    } else {
      for (const llvm::APInt &Value : High)
        Coefficients.push_back(Value - Low[0]);
    }
    for (const llvm::APInt &Coefficient : Coefficients) {
      if (Coefficient.isZero())
        continue;
      if (!Budget.consume(Weights.size()))
        return;
      llvm::SmallVector<llvm::APInt, 2> ResidualValues;
      TruthTable Selector = TruthTable::zero(Count);
      bool TwoValued = true;
      for (size_t K = 0; K < Weights.size(); ++K) {
        llvm::APInt Value = Weights[K];
        if (K & (size_t(1) << Axis))
          Value -= Coefficient;
        if (ResidualValues.empty())
          ResidualValues.push_back(Value);
        if (Value == ResidualValues[0])
          continue;
        if (ResidualValues.size() == 1)
          ResidualValues.push_back(Value);
        if (Value != ResidualValues[1]) {
          TwoValued = false;
          break;
        }
        Selector.set(K);
      }
      if (!TwoValued || ResidualValues.size() != 2)
        continue;

      for (unsigned Orientation = 0; Orientation < 2; ++Orientation) {
        // Pay for each construction, and prepay the small exact table on its
        // first lookup in this region. Reusing an already-built selector does
        // not restart its synthesis allowance.
        if (!Budget.consume(Weights.size()))
          return;
        const TruthTable Table = Orientation == 0 ? Selector : ~Selector;
        const uint32_t Key = static_cast<uint32_t>(Table.packed());
        auto Found = Synthesized.find(Key);
        if (Found == Synthesized.end()) {
          const unsigned Arity = std::popcount(truthTableSupport(Table));
          const size_t Functions = size_t(1) << (size_t(1) << Arity);
          const size_t SynthesisWork = Functions * Functions;
          if (SynthesisWork > Limits.SynthesisWork)
            continue;
          if (!Budget.consume(SynthesisWork))
            return;
          BitwiseSynthesisLimits Synthesis = Limits.synthesis(TermBudget);
          Synthesis.MaxOptimalAtoms = Count;
          Synthesis.MaxWork = SynthesisWork;
          std::optional<SymRef> Boolean =
              synthesizeBitwise(Ctx, Table, Atoms, Synthesis);
          Found =
              Synthesized.try_emplace(Key, Boolean.value_or(SymRef())).first;
        }
        if (!Found->second.isValid())
          continue;
        const llvm::APInt &Base = ResidualValues[Orientation];
        const llvm::APInt &Other = ResidualValues[1 - Orientation];
        SymRef Form =
            Ctx.mkAdd({Ctx.mkConst(-Base),
                       Ctx.mkMul(Ctx.mkConst(Other - Base), Found->second),
                       Ctx.mkMul(Ctx.mkConst(Coefficient), Atoms[Axis])});
        // With offset -1, reversing the two coefficients under a complement
        // spells the same word: ~(-a*B-c*X) = -1+a*B+c*X. Reuse the proven
        // selector and keep one form per orientation; no new search is needed.
        if (Base.isOne() && Budget.consume(Weights.size())) {
          SymRef Complement = Ctx.mkNot(
              Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(Base - Other), Found->second),
                        Ctx.mkMul(Ctx.mkConst(-Coefficient), Atoms[Axis])));
          if (readingScore(Ctx, Complement) < readingScore(Ctx, Form))
            Form = Complement;
        }
        if (readingCost(Ctx, Form) <= TermBudget &&
            !llvm::is_contained(Out, Form))
          Out.push_back(Form);
      }
    }
  }
}

void booleanResidualCandidates(SymContext &Ctx,
                               llvm::ArrayRef<llvm::APInt> Weights,
                               llvm::ArrayRef<SymRef> Atoms, size_t TermBudget,
                               const SolverLimits &Limits, WorkBudget &Budget,
                               llvm::SmallVectorImpl<SymRef> &Out) {
  const unsigned Count = static_cast<unsigned>(Atoms.size());
  // With two effective inputs, four different weights require both selectors
  // to be balanced. The balanced tables are the inputs, their complements,
  // XOR and its complement. Two XOR tables cannot give four value pairs, so
  // one selector is an affine atom already covered by the earlier reading.
  if (Count < 3 ||
      Count > std::min({3u, Limits.MaxOptimalSynthesisAtoms,
                        Limits.MaxSynthesisAtoms}) ||
      TermBudget < 3)
    return;
  assert(Weights.size() == (size_t(1) << Count));
  if (!Budget.consume(Weights.size()))
    return;
  llvm::SmallVector<llvm::APInt, 4> Values;
  for (const llvm::APInt &Value : Weights) {
    if (llvm::is_contained(Values, Value))
      continue;
    if (Values.size() == 4)
      return;
    Values.push_back(Value);
  }
  // Repeated values leave choices in how a selector should split a group.
  // This bounded reading handles only the unambiguous four-value case.
  if (Values.size() != 4)
    return;
  // Four values have only three possible pairs of opposite corners. Reject
  // nonrectangular tables before computing support or constructing selectors.
  if (!Budget.consume(3))
    return;
  if (Values[0] + Values[1] != Values[2] + Values[3] &&
      Values[0] + Values[2] != Values[1] + Values[3] &&
      Values[0] + Values[3] != Values[1] + Values[2])
    return;
  if (!Budget.consume(Count * Weights.size()))
    return;
  unsigned Support = 0;
  for (unsigned Axis = 0; Axis < Count; ++Axis) {
    const size_t Bit = size_t(1) << Axis;
    for (size_t K = 0; K < Weights.size(); ++K)
      if (!(K & Bit) && Weights[K] != Weights[K | Bit]) {
        ++Support;
        break;
      }
  }
  if (Support < 3 || TermBudget < 2 * Support - 1)
    return;

  llvm::DenseMap<uint32_t, SymRef> Synthesized;
  auto selector = [&](const TruthTable &Table) -> std::optional<SymRef> {
    const uint32_t Key = static_cast<uint32_t>(Table.packed());
    auto Found = Synthesized.find(Key);
    if (Found == Synthesized.end()) {
      const unsigned Arity = std::popcount(truthTableSupport(Table));
      const size_t Functions = size_t(1) << (size_t(1) << Arity);
      const size_t Work = Functions * Functions;
      if (Work > Limits.SynthesisWork)
        return std::nullopt;
      if (!Budget.consume(Work))
        return std::nullopt;
      BitwiseSynthesisLimits Synthesis = Limits.synthesis(TermBudget);
      Synthesis.MaxOptimalAtoms = Count;
      Synthesis.MaxWork = Work;
      auto Built = synthesizeBitwise(Ctx, Table, Atoms, Synthesis);
      Found = Synthesized.try_emplace(Key, Built.value_or(SymRef())).first;
    }
    if (!Found->second.isValid())
      return std::nullopt;
    return Found->second;
  };

  // Label the four values as 00, 10, 01, 11. They form an additive rectangle
  // precisely when opposite sums agree modulo the word width. No division is
  // involved, so even coefficients and wrapping sums follow the same rule.
  // There are 24 labelings and only six possible selectors: each joins two
  // of the four value groups. Cache those tables for this reading.
  std::array<unsigned, 4> Order{0, 1, 2, 3};
  do {
    if (!Budget.consume())
      return;
    const llvm::APInt &Base = Values[Order[0]];
    if (Base + Values[Order[3]] != Values[Order[1]] + Values[Order[2]])
      continue;
    if (!Budget.consume(Weights.size()))
      return;
    TruthTable Left = TruthTable::zero(Count);
    TruthTable Right = TruthTable::zero(Count);
    for (size_t K = 0; K < Weights.size(); ++K) {
      if (Weights[K] == Values[Order[1]] || Weights[K] == Values[Order[3]])
        Left.set(K);
      if (Weights[K] == Values[Order[2]] || Weights[K] == Values[Order[3]])
        Right.set(K);
    }
    if (!Budget.consume(2 * Count * Weights.size()))
      return;
    if (std::popcount(truthTableSupport(Left)) < 2 ||
        std::popcount(truthTableSupport(Right)) < 2)
      continue;
    auto L = selector(Left);
    if (!L)
      continue;
    auto R = selector(Right);
    if (!R)
      continue;
    if (!Budget.consume(Weights.size()))
      return;
    SymRef Form =
        Ctx.mkAdd({Ctx.mkConst(-Base),
                   Ctx.mkMul(Ctx.mkConst(Values[Order[1]] - Base), *L),
                   Ctx.mkMul(Ctx.mkConst(Values[Order[2]] - Base), *R)});
    if (readingCost(Ctx, Form) <= TermBudget && !llvm::is_contained(Out, Form))
      Out.push_back(Form);
  } while (std::next_permutation(Order.begin(), Order.end()));
}

SymRef cheapestOf(const SymContext &Ctx, llvm::ArrayRef<SymRef> Candidates) {
  SymRef Best;
  SymReadability BestCost;
  for (SymRef Candidate : Candidates) {
    SymReadability Cost = readingScore(Ctx, Candidate);
    if (!Best.isValid() || Cost < BestCost) {
      Best = Candidate;
      BestCost = Cost;
    }
  }
  return Best;
}

} // namespace neverd::symbolic::detail
