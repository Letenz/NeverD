//===- SymMBAArithmetic.cpp - Arithmetic over fixed-width words ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SymMBADetail.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace neverd::symbolic::detail {
namespace {

using Factor = std::pair<uint32_t, uint32_t>;
using Monomial = std::vector<Factor>;
using Polynomial = std::map<Monomial, llvm::APInt>;

struct CoefficientLess {
  bool operator()(const llvm::APInt &A, const llvm::APInt &B) const {
    return A.ult(B);
  }
};

// Allocation charges are conservative and cumulative for one attempt. Keeping
// even released storage charged bounds all temporary tables and candidate
// construction together, without making reclamation order part of the policy.
class ArithmeticResources {
  WorkBudget &Work;
  size_t Bytes;
  bool Stopped = false;

public:
  ArithmeticResources(WorkBudget &Work, size_t Bytes)
      : Work(Work), Bytes(Bytes) {}

  bool charge(size_t Units, size_t Storage = 0) {
    if (Stopped)
      return false;
    if (!Work.consume(Units) || Storage > Bytes) {
      Stopped = true;
      return false;
    }
    Bytes -= Storage;
    return true;
  }

  bool stop() {
    Stopped = true;
    return false;
  }

  bool array(size_t Count, size_t ElementBytes) {
    if (ElementBytes &&
        Count > std::numeric_limits<size_t>::max() / ElementBytes)
      return stop();
    return charge(Count, Count * ElementBytes);
  }

  bool stopped() const { return Stopped; }
};

class ArithmeticNormalizer {
  SymContext &Ctx;
  ArithmeticResources &Resources;
  uint32_t Width;
  size_t Words;

public:
  ArithmeticNormalizer(SymContext &Ctx, ArithmeticResources &Resources,
                       uint32_t Width)
      : Ctx(Ctx), Resources(Resources), Width(Width),
        Words((size_t(Width) + 63) / 64) {}

  bool add(Polynomial &To, const Monomial &Key,
           const llvm::APInt &Coefficient) {
    if (!Resources.charge(1 + Key.size() + Words))
      return false;
    if (Coefficient.isZero())
      return true;
    auto It = To.find(Key);
    if (It != To.end()) {
      It->second += Coefficient;
      if (It->second.isZero())
        To.erase(It);
      return true;
    }
    if (!Resources.charge(1, 128) ||
        !Resources.array(Key.size(), sizeof(Factor)) ||
        !Resources.array(Words, sizeof(uint64_t)))
      return false;
    To.emplace(Key, Coefficient);
    return true;
  }

  bool copy(const Polynomial &From, Polynomial &To) {
    for (const auto &[Key, Coefficient] : From)
      if (!add(To, Key, Coefficient))
        return false;
    return true;
  }

  bool multiply(const Polynomial &A, const Polynomial &B, Polynomial &Out) {
    for (const auto &[AK, AC] : A) {
      for (const auto &[BK, BC] : B) {
        if (Words > std::numeric_limits<size_t>::max() / Words ||
            AK.size() > std::numeric_limits<size_t>::max() - BK.size())
          return Resources.stop();
        if (!Resources.charge(Words * Words) ||
            !Resources.array(Words, sizeof(uint64_t)) ||
            !Resources.array(AK.size() + BK.size(), sizeof(Factor)))
          return false;
        llvm::APInt Coefficient = AC * BC;
        if (Coefficient.isZero())
          continue;
        Monomial Key;
        Key.reserve(AK.size() + BK.size());
        size_t I = 0, J = 0;
        while (I != AK.size() || J != BK.size()) {
          if (J == BK.size() || (I != AK.size() && AK[I].first < BK[J].first))
            Key.push_back(AK[I++]);
          else if (I == AK.size() || BK[J].first < AK[I].first)
            Key.push_back(BK[J++]);
          else {
            const uint64_t Power = uint64_t(AK[I].second) + BK[J].second;
            if (Power > std::numeric_limits<uint32_t>::max())
              return Resources.stop();
            Key.emplace_back(AK[I].first, static_cast<uint32_t>(Power));
            ++I;
            ++J;
          }
        }
        if (!add(Out, Key, Coefficient))
          return false;
      }
    }
    return true;
  }

  std::optional<Polynomial> normalize(SymRef Root,
                                      unsigned *AtomCount = nullptr) {
    if (!Resources.array(Words, sizeof(uint64_t)))
      return std::nullopt;
    std::set<uint32_t> Reachable;
    std::vector<SymRef> Pending;
    if (!Resources.charge(1, 128))
      return std::nullopt;
    Pending.push_back(Root);
    while (!Pending.empty()) {
      SymRef R = Pending.back();
      Pending.pop_back();
      if (!Resources.charge(1))
        return std::nullopt;
      if (Reachable.contains(R.index()))
        continue;
      if (!Resources.charge(1, 128))
        return std::nullopt;
      Reachable.insert(R.index());
      if (Ctx.width(R) != Width)
        return std::nullopt;
      if (Ctx.op(R) != SymOp::Add && Ctx.op(R) != SymOp::Mul &&
          Ctx.op(R) != SymOp::Not)
        continue;
      llvm::ArrayRef<SymRef> Children = Ctx.operands(R);
      if (!Resources.array(Children.size(), sizeof(SymRef)))
        return std::nullopt;
      Pending.insert(Pending.end(), Children.begin(), Children.end());
    }

    // Node identities are topological: an interned node is newer than every
    // operand. Opaque nodes deliberately stop the walk, even if their own
    // subexpressions contain arithmetic or have a different width.
    std::map<uint32_t, Polynomial> Memo;
    unsigned Atoms = 0;
    for (uint32_t Index : Reachable) {
      if (!Resources.charge(1, 128))
        return std::nullopt;
      SymRef R(Index);
      Polynomial P;
      if (Ctx.isConst(R)) {
        if (!add(P, {}, Ctx.constValue(R)))
          return std::nullopt;
      } else if (Ctx.op(R) == SymOp::Add) {
        for (SymRef Child : Ctx.operands(R))
          if (!copy(Memo.at(Child.index()), P))
            return std::nullopt;
      } else if (Ctx.op(R) == SymOp::Mul) {
        if (!add(P, {}, llvm::APInt(Width, 1)))
          return std::nullopt;
        for (SymRef Child : Ctx.operands(R)) {
          Polynomial Next;
          if (!multiply(P, Memo.at(Child.index()), Next))
            return std::nullopt;
          P = std::move(Next);
          if (P.empty())
            break;
        }
      } else if (Ctx.op(R) == SymOp::Not) {
        // A word complement is exactly -1-X in the modular ring. Only
        // arithmetic paths reach this node: bitwise consumers remain opaque.
        if (!Resources.array(Words, sizeof(uint64_t)) ||
            !add(P, {}, llvm::APInt::getAllOnes(Width)))
          return std::nullopt;
        for (const auto &[Key, Coefficient] :
             Memo.at(Ctx.operand(R, 0).index())) {
          if (!Resources.array(Words, sizeof(uint64_t)) ||
              !add(P, Key, -Coefficient))
            return std::nullopt;
        }
      } else {
        ++Atoms;
        if (!add(P, {{Index, 1}}, llvm::APInt(Width, 1)))
          return std::nullopt;
      }
      Memo.emplace(Index, std::move(P));
    }
    if (AtomCount)
      *AtomCount = Atoms;
    return std::move(Memo.at(Root.index()));
  }

  std::optional<SymRef> combine(SymOp Op, llvm::ArrayRef<SymRef> Terms) {
    size_t Count = 0;
    for (SymRef Term : Terms) {
      const size_t N = Ctx.op(Term) == Op ? Ctx.numOperands(Term) : 1;
      if (N > std::numeric_limits<size_t>::max() - Count)
        return std::nullopt;
      Count += N;
    }
    if (!Resources.charge(1, 128) || !Resources.array(Count, 32))
      return std::nullopt;
    return Op == SymOp::Add ? Ctx.mkAdd(Terms) : Ctx.mkMul(Terms);
  }

  std::optional<SymRef> term(const Monomial &Key,
                             const llvm::APInt &Coefficient) {
    size_t Count = !Coefficient.isOne();
    for (const Factor &F : Key) {
      if (F.second > std::numeric_limits<size_t>::max() - Count)
        return std::nullopt;
      Count += F.second;
    }
    if (!Resources.charge(1, 128) || !Resources.array(Count, sizeof(SymRef)))
      return std::nullopt;
    llvm::SmallVector<SymRef, 8> Factors;
    Factors.reserve(Count);
    if (!Coefficient.isOne() || Key.empty())
      Factors.push_back(Ctx.mkConst(Coefficient));
    for (const auto &[Id, Power] : Key)
      Factors.append(Power, SymRef(Id));
    return combine(SymOp::Mul, Factors);
  }

  std::optional<SymRef> expanded(const Polynomial &P) {
    if (!Resources.array(P.size(), sizeof(SymRef)))
      return std::nullopt;
    llvm::SmallVector<SymRef, 8> Terms;
    for (const auto &[Key, Coefficient] : P) {
      std::optional<SymRef> T = term(Key, Coefficient);
      if (!T)
        return std::nullopt;
      Terms.push_back(*T);
    }
    if (Terms.empty())
      return Ctx.mkZero(Width);
    return combine(SymOp::Add, Terms);
  }

  std::optional<SymRef> factored(const Polynomial &Source) {
    struct Frame {
      Polynomial P, Rest;
      SymRef Factor, Product;
      unsigned Stage = 0;
    };
    std::vector<Frame> Stack;
    Frame Initial;
    if (!copy(Source, Initial.P) || !Resources.charge(1, sizeof(Frame)))
      return std::nullopt;
    Stack.push_back(std::move(Initial));
    SymRef Result;
    while (!Stack.empty()) {
      Frame &F = Stack.back();
      if (F.Stage == 1) {
        auto Product = combine(SymOp::Mul, {F.Factor, Result});
        if (!Product)
          return std::nullopt;
        F.Product = *Product;
        if (F.Rest.empty()) {
          Result = F.Product;
          Stack.pop_back();
        } else {
          F.Stage = 2;
          Frame Next;
          Next.P = std::move(F.Rest);
          if (!Resources.charge(1, sizeof(Frame)))
            return std::nullopt;
          Stack.push_back(std::move(Next));
        }
        continue;
      }
      if (F.Stage == 2) {
        auto Sum = combine(SymOp::Add, {F.Product, Result});
        if (!Sum)
          return std::nullopt;
        Result = *Sum;
        Stack.pop_back();
        continue;
      }

      // Choose an atom shared by several terms. Removing the smallest power
      // among those terms is exact even when other terms do not contain it.
      std::map<uint32_t, std::pair<size_t, uint32_t>> Counts;
      for (const auto &[Key, Coefficient] : F.P) {
        (void)Coefficient;
        for (const auto &[Id, Power] : Key) {
          if (!Resources.charge(1, 64))
            return std::nullopt;
          auto [It, Inserted] = Counts.try_emplace(Id, 0, Power);
          ++It->second.first;
          It->second.second = std::min(It->second.second, Power);
        }
      }
      uint32_t BestId = 0, BestPower = 0;
      uint64_t BestScore = 0;
      for (const auto &[Id, Count] : Counts) {
        if (Count.first < 2)
          continue;
        const uint64_t N = Count.first - 1;
        const uint64_t Score =
            N > std::numeric_limits<uint64_t>::max() / Count.second
                ? std::numeric_limits<uint64_t>::max()
                : N * Count.second;
        if (Score > BestScore) {
          BestScore = Score;
          BestId = Id;
          BestPower = Count.second;
        }
      }
      if (!BestPower) {
        // Equal coefficients can be removed without division in the modular
        // ring, including even coefficients that have no multiplicative
        // inverse. Positive units need no factor; negative units retain the
        // established sign spelling used by the repeated region walk.
        std::map<llvm::APInt, size_t, CoefficientLess> Coefficients;
        for (const auto &[Key, Coefficient] : F.P) {
          if (Key.empty() || Coefficient.isOne() || Coefficient.isAllOnes())
            continue;
          if (!Resources.charge(1, 64) ||
              !Resources.array(Words, sizeof(uint64_t)))
            return std::nullopt;
          ++Coefficients[Coefficient];
        }
        auto Best = Coefficients.end();
        for (auto It = Coefficients.begin(); It != Coefficients.end(); ++It)
          if (It->second >= 2 &&
              (Best == Coefficients.end() || It->second > Best->second))
            Best = It;
        if (Best != Coefficients.end()) {
          Frame Next;
          if (!Resources.array(Words, sizeof(uint64_t)))
            return std::nullopt;
          const llvm::APInt One(Width, 1);
          for (const auto &[Key, Coefficient] : F.P) {
            if (Coefficient == Best->first) {
              if (!add(Next.P, Key, One))
                return std::nullopt;
            } else if (!add(F.Rest, Key, Coefficient)) {
              return std::nullopt;
            }
          }
          if (!Resources.charge(1, sizeof(Frame) + 128) ||
              !Resources.array(Words, sizeof(uint64_t)))
            return std::nullopt;
          F.Factor = Ctx.mkConst(Best->first);
          F.P.clear();
          F.Stage = 1;
          Stack.push_back(std::move(Next));
          continue;
        }
        auto Leaf = expanded(F.P);
        if (!Leaf)
          return std::nullopt;
        Result = *Leaf;
        Stack.pop_back();
        continue;
      }

      Frame Next;
      for (const auto &[Key, Coefficient] : F.P) {
        auto It = std::lower_bound(Key.begin(), Key.end(), Factor{BestId, 0});
        if (It == Key.end() || It->first != BestId) {
          if (!add(F.Rest, Key, Coefficient))
            return std::nullopt;
          continue;
        }
        if (!Resources.array(Key.size(), sizeof(Factor)))
          return std::nullopt;
        Monomial Quotient(Key);
        const size_t Index = static_cast<size_t>(It - Key.begin());
        Quotient[Index].second -= BestPower;
        if (!Quotient[Index].second)
          Quotient.erase(Quotient.begin() + Index);
        if (!add(Next.P, Quotient, Coefficient))
          return std::nullopt;
      }
      auto Common = term({{BestId, BestPower}}, llvm::APInt(Width, 1));
      if (!Common || !Resources.charge(1, sizeof(Frame)))
        return std::nullopt;
      F.P.clear();
      F.Factor = *Common;
      F.Stage = 1;
      Stack.push_back(std::move(Next));
    }
    return Result;
  }
};

bool mayImproveArithmetic(const SymContext &Ctx, SymRef Root) {
  if (Ctx.op(Root) != SymOp::Add && Ctx.op(Root) != SymOp::Mul)
    return false;
  if (Ctx.op(Root) == SymOp::Mul)
    return llvm::any_of(Ctx.operands(Root),
                        [&](SymRef R) { return Ctx.op(R) == SymOp::Add; });
  // Linear regions already handle complements of arithmetic expressions.
  // Expanding them here as the sole reason for an attempt repeatedly reads
  // growing tails during a layered walk. A complemented atom can expose a
  // new cancellation; nonlinear products below still admit compound words.
  auto ExposesComplementAtom = [&](SymRef R) {
    if (Ctx.op(R) != SymOp::Not)
      return false;
    SymOp Inner = Ctx.op(Ctx.operand(R, 0));
    return Inner != SymOp::Add && Inner != SymOp::Mul;
  };
  for (SymRef Term : Ctx.operands(Root)) {
    if (ExposesComplementAtom(Term))
      return true;
    if (Ctx.op(Term) != SymOp::Mul)
      continue;
    unsigned NonConstants = 0;
    for (SymRef Factor : Ctx.operands(Term)) {
      if (Ctx.op(Factor) == SymOp::Add || ExposesComplementAtom(Factor))
        return true;
      if (!Ctx.isConst(Factor) && ++NonConstants == 2)
        return true;
    }
  }
  return false;
}

SymRef normalizeArithmetic(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                           WorkBudget &Budget, SolveReport &Rep) {
  ArithmeticResources Resources(Budget, Opts.MaxTableBytes);
  ArithmeticNormalizer Normalizer(Ctx, Resources, Ctx.width(E));
  auto Stop = [&]() {
    if (Resources.stopped() || Budget.exhausted()) {
      Rep.BudgetExhausted = true;
      Rep.Outcome = MBAOutcome::BudgetExhausted;
    }
    return E;
  };
  unsigned Atoms = 0;
  std::optional<Polynomial> Original = Normalizer.normalize(E, &Atoms);
  if (!Original)
    return Stop();
  auto Expanded = Normalizer.expanded(*Original);
  if (!Expanded)
    return Stop();
  SymRef Best = *Expanded;
  if (Original->size() > 1) {
    auto Factored = Normalizer.factored(*Original);
    if (!Factored)
      return Stop();
    if (readingScore(Ctx, *Factored) < readingScore(Ctx, Best))
      Best = *Factored;
  }
  if (Best == E || readingScore(Ctx, Best) >= readingScore(Ctx, E))
    return E;

  // Re-expand the chosen spelling: factor selection and emission are not
  // trusted to preserve the polynomial merely because they produced it.
  std::optional<Polynomial> Check = Normalizer.normalize(Best);
  if (!Check)
    return Stop();
  if (*Check != *Original || !agreeOnSamples(Ctx, E, Best, Opts.VerifySamples))
    return E;
  Rep.NumAtoms = Atoms;
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Best;
}

bool hasRepeatedCoefficients(const SymContext &Ctx, SymRef E) {
  if (Ctx.op(E) != SymOp::Add)
    return false;
  // Constant identities admit repeated coefficients without allocating a
  // table. Collisions only admit an extra budgeted normalization attempt.
  uint64_t Seen = 0;
  for (SymRef Term : Ctx.operands(E)) {
    if (Ctx.op(Term) != SymOp::Mul)
      continue;
    SymRef Factor = Ctx.operand(Term, 0);
    if (!Ctx.isConst(Factor))
      continue;
    // Wide values are not copied before the resource guard is established.
    if (Ctx.width(Factor) <= 64 &&
        (Ctx.constValue(Factor).isOne() || Ctx.constValue(Factor).isAllOnes()))
      continue;
    const uint64_t Bit = uint64_t(1) << (Factor.index() % 64);
    if (Seen & Bit)
      return true;
    Seen |= Bit;
  }
  return false;
}

} // namespace

SymRef solveArithmetic(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                       WorkBudget &Budget, SolveReport &Rep) {
  return mayImproveArithmetic(Ctx, E)
             ? normalizeArithmetic(Ctx, E, Opts, Budget, Rep)
             : E;
}

SymRef solveCoefficientFactors(SymContext &Ctx, SymRef E,
                               const MBAOptions &Opts, WorkBudget &Budget,
                               SolveReport &Rep) {
  return hasRepeatedCoefficients(Ctx, E)
             ? normalizeArithmetic(Ctx, E, Opts, Budget, Rep)
             : E;
}

} // namespace neverd::symbolic::detail
