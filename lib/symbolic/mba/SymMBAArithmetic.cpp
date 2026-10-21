//===- SymMBAArithmetic.cpp - Arithmetic over fixed-width words ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "Limits.h"
#include "SymMBADetail.h"

#include "llvm/ADT/ScopeExit.h"

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
  size_t remainingBytes() const { return Bytes; }
};

class ArithmeticNormalizer {
  SymContext &Ctx;
  ArithmeticResources &Resources;
  uint32_t Width;
  size_t Words;
  bool ExpandComplements;
  // Completed polynomials belong to this one context, width and complement
  // policy. std::map keeps returned references stable while later roots grow
  // the memo. Candidate emission never writes these checked coefficients.
  std::map<uint32_t, Polynomial> Memo;

public:
  ArithmeticNormalizer(SymContext &Ctx, ArithmeticResources &Resources,
                       uint32_t Width, bool ExpandComplements = true)
      : Ctx(Ctx), Resources(Resources), Width(Width),
        Words((size_t(Width) + 63) / 64), ExpandComplements(ExpandComplements) {
  }

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

  // These keys and coefficients have already been allocated and charged by
  // multiplication or flat-monomial construction. Transfer their ownership
  // instead of allocating and copying the same polynomial entry again.
  bool addOwned(Polynomial &To, Monomial Key, llvm::APInt Coefficient) {
    if (!Resources.charge(1 + Key.size() + Words))
      return false;
    if (Coefficient.isZero())
      return true;
    auto It = To.lower_bound(Key);
    if (It != To.end() && It->first == Key) {
      It->second += Coefficient;
      if (It->second.isZero())
        To.erase(It);
      return true;
    }
    if (!Resources.charge(1, 128))
      return false;
    To.emplace_hint(It, std::move(Key), std::move(Coefficient));
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
        if (!addOwned(Out, std::move(Key), std::move(Coefficient)))
          return false;
      }
    }
    return true;
  }

  const Polynomial *normalize(SymRef Root, unsigned *AtomCount = nullptr) {
    if (!Resources.array(Words, sizeof(uint64_t)))
      return nullptr;
    const bool ReuseMemo = !Memo.empty();
    std::set<uint32_t> Reachable;
    std::vector<SymRef> Pending;
    if (!Resources.charge(1, 128))
      return nullptr;
    Pending.push_back(Root);
    while (!Pending.empty()) {
      SymRef R = Pending.back();
      Pending.pop_back();
      if (!Resources.charge(1))
        return nullptr;
      if (Reachable.contains(R.index()))
        continue;
      if (ReuseMemo && !AtomCount && Memo.contains(R.index()))
        continue;
      if (!Resources.charge(1, 128))
        return nullptr;
      Reachable.insert(R.index());
      if (Ctx.width(R) != Width)
        return nullptr;
      if (Ctx.op(R) != SymOp::Add && Ctx.op(R) != SymOp::Mul &&
          (Ctx.op(R) != SymOp::Not || !ExpandComplements))
        continue;
      llvm::ArrayRef<SymRef> Children = Ctx.operands(R);
      if (!Resources.array(Children.size(), sizeof(SymRef)))
        return nullptr;
      Pending.insert(Pending.end(), Children.begin(), Children.end());
    }

    // Node identities are topological: an interned node is newer than every
    // operand. Opaque nodes deliberately stop the walk, even if their own
    // subexpressions contain arithmetic or have a different width.
    unsigned Atoms = 0;
    for (uint32_t Index : Reachable) {
      if (ReuseMemo && Memo.contains(Index)) {
        SymOp Op = Ctx.op(SymRef(Index));
        Atoms += !Ctx.isConst(SymRef(Index)) && Op != SymOp::Add &&
                 Op != SymOp::Mul && (Op != SymOp::Not || !ExpandComplements);
        continue;
      }
      if (!Resources.charge(1, 128))
        return nullptr;
      SymRef R(Index);
      Polynomial P;
      if (Ctx.isConst(R)) {
        if (!add(P, {}, Ctx.constValue(R)))
          return nullptr;
      } else if (Ctx.op(R) == SymOp::Add) {
        for (SymRef Child : Ctx.operands(R))
          if (!copy(Memo.at(Child.index()), P))
            return nullptr;
      } else if (Ctx.op(R) == SymOp::Mul) {
        // Long flat products already name a sorted monomial. Read it once
        // instead of multiplying unit coefficients and merging one atom at
        // a time. Arithmetic children retain the general expansion below.
        bool Flat = Ctx.numOperands(R) >= 6;
        if (Flat)
          for (SymRef Child : Ctx.operands(R)) {
            if (!Resources.charge(1))
              return nullptr;
            SymOp Op = Ctx.op(Child);
            if (Op == SymOp::Add || Op == SymOp::Mul ||
                (Op == SymOp::Not && ExpandComplements)) {
              Flat = false;
              break;
            }
          }
        if (Flat) {
          if (!Resources.array(Ctx.numOperands(R), sizeof(Factor)) ||
              !Resources.array(Words, sizeof(uint64_t)))
            return nullptr;
          Monomial Key;
          Key.reserve(Ctx.numOperands(R));
          llvm::APInt Coefficient(Width, 1);
          for (SymRef Child : Ctx.operands(R)) {
            if (Ctx.isConst(Child)) {
              if (Words > std::numeric_limits<size_t>::max() / Words ||
                  !Resources.charge(Words * Words))
                return nullptr;
              Coefficient *= Ctx.constValue(Child);
            } else if (!Key.empty() && Key.back().first == Child.index()) {
              if (Key.back().second == std::numeric_limits<uint32_t>::max())
                return nullptr;
              ++Key.back().second;
            } else {
              Key.emplace_back(Child.index(), 1);
            }
          }
          if (!addOwned(P, std::move(Key), std::move(Coefficient)))
            return nullptr;
          Memo.emplace(Index, std::move(P));
          continue;
        }
        // Assemble monomial operands once. Repeatedly multiplying one more
        // atom copies the growing key at every step, making a flat product's
        // distribution proof quadratic in its number of factors.
        llvm::APInt Scale(Width, 1);
        std::map<uint32_t, uint32_t> Powers;
        unsigned PolynomialOperands = 0;
        for (SymRef Child : Ctx.operands(R)) {
          const auto &Operand = Memo.at(Child.index());
          if (Operand.empty()) {
            Scale.clearAllBits();
            break;
          }
          if (Operand.size() != 1) {
            ++PolynomialOperands;
            continue;
          }
          const auto &[Key, Coefficient] = *Operand.begin();
          if (!Resources.charge(Words))
            return nullptr;
          if (!Coefficient.isOne()) {
            if (Words > std::numeric_limits<size_t>::max() / Words ||
                !Resources.charge(Words * Words) ||
                !Resources.array(Words, sizeof(uint64_t)))
              return nullptr;
            Scale *= Coefficient;
          }
          for (const auto &[Id, Power] : Key) {
            if (!Resources.charge(1, 128))
              return nullptr;
            auto &Total = Powers[Id];
            if (Power > std::numeric_limits<uint32_t>::max() - Total)
              return nullptr;
            Total += Power;
          }
        }
        if (!Resources.array(Powers.size(), sizeof(Factor)))
          return nullptr;
        Monomial Key(Powers.begin(), Powers.end());
        // Multiply the polynomial children before attaching a long common
        // monomial. Its powers then need copying only once per final term.
        // Keep the scale early so modular zero coefficients still prune work.
        const bool DelayCommon =
            Powers.size() >= limits::kMinDelayedCommonAtoms &&
            PolynomialOperands >= limits::kMinDelayedPolynomialOperands;
        if (!add(P, DelayCommon ? Monomial{} : Key, Scale))
          return nullptr;
        for (SymRef Child : Ctx.operands(R)) {
          if (P.empty())
            break;
          if (Memo.at(Child.index()).size() == 1)
            continue;
          Polynomial Next;
          if (!multiply(P, Memo.at(Child.index()), Next))
            return nullptr;
          P = std::move(Next);
          if (P.empty())
            break;
        }
        if (DelayCommon && !P.empty()) {
          Polynomial Common, Next;
          if (!add(Common, Key, llvm::APInt(Width, 1)) ||
              !multiply(P, Common, Next))
            return nullptr;
          P = std::move(Next);
        }
      } else if (Ctx.op(R) == SymOp::Not && ExpandComplements) {
        // A word complement is exactly -1-X in the modular ring. Only
        // arithmetic paths reach this node: bitwise consumers remain opaque.
        if (!Resources.array(Words, sizeof(uint64_t)) ||
            !add(P, {}, llvm::APInt::getAllOnes(Width)))
          return nullptr;
        for (const auto &[Key, Coefficient] :
             Memo.at(Ctx.operand(R, 0).index())) {
          if (!Resources.array(Words, sizeof(uint64_t)) ||
              !add(P, Key, -Coefficient))
            return nullptr;
        }
      } else {
        ++Atoms;
        if (!add(P, {{Index, 1}}, llvm::APInt(Width, 1)))
          return nullptr;
      }
      Memo.emplace(Index, std::move(P));
    }
    if (AtomCount)
      *AtomCount = Atoms;
    return &Memo.at(Root.index());
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

      // Extract an entire shared monomial together. Removing just one atom
      // per frame repeatedly scans and copies the same long common powers.
      Monomial Divisor;
      if (Counts.at(BestId).first == F.P.size()) {
        for (const auto &[Id, Count] : Counts)
          if (Count.first == F.P.size()) {
            if (!Resources.array(1, sizeof(Factor)))
              return std::nullopt;
            Divisor.emplace_back(Id, Count.second);
          }
      } else {
        if (!Resources.array(1, sizeof(Factor)))
          return std::nullopt;
        Divisor.emplace_back(BestId, BestPower);
      }
      Frame Next;
      for (const auto &[Key, Coefficient] : F.P) {
        auto It = std::lower_bound(Key.begin(), Key.end(), Factor{BestId, 0});
        if (It == Key.end() || It->first != BestId) {
          if (!add(F.Rest, Key, Coefficient))
            return std::nullopt;
          continue;
        }
        if (!Resources.array(Key.size(), sizeof(Factor)) ||
            !Resources.array(Words, sizeof(uint64_t)))
          return std::nullopt;
        Monomial Quotient;
        Quotient.reserve(Key.size());
        size_t I = 0;
        for (auto [Id, Power] : Key) {
          if (I != Divisor.size() && Divisor[I].first == Id)
            Power -= Divisor[I++].second;
          if (Power)
            Quotient.emplace_back(Id, Power);
        }
        if (!addOwned(Next.P, std::move(Quotient), Coefficient))
          return std::nullopt;
      }
      auto Common = term(Divisor, llvm::APInt(Width, 1));
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
                           WorkBudget &Budget, SolveReport &Rep,
                           bool ExpandComplements = true) {
  ArithmeticResources Resources(Budget, Opts.MaxTableBytes);
  ArithmeticNormalizer Normalizer(Ctx, Resources, Ctx.width(E),
                                  ExpandComplements);
  auto Stop = [&]() {
    if (Resources.stopped() || Budget.exhausted()) {
      Rep.BudgetExhausted = true;
      Rep.Outcome = MBAOutcome::BudgetExhausted;
    }
    return E;
  };
  unsigned Atoms = 0;
  const auto *Original = Normalizer.normalize(E, &Atoms);
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
  const auto *Check = Normalizer.normalize(Best);
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

// Strip an identical nonconstant monomial from both sides of a large
// distribution proof. This proves the sufficient identity between quotients;
// it never divides a word or assumes that a modular coefficient is invertible.
// Keeping complements opaque makes these factors formal polynomial atoms.
std::pair<SymRef, SymRef>
reduceDistributionProof(SymContext &Ctx, SymRef Before, SymRef After,
                        ArithmeticResources &Resources) {
  const auto Original = std::make_pair(Before, After);
  constexpr size_t MaxTerms = 128, MaxEdges = 4096;
  if (Ctx.op(Before) != SymOp::Add || Ctx.numOperands(Before) < 6 ||
      Ctx.numOperands(Before) > MaxTerms ||
      (Ctx.op(After) == SymOp::Add && Ctx.numOperands(After) > MaxTerms))
    return Original;
  size_t Count = Ctx.numOperands(Before) +
                 (Ctx.op(After) == SymOp::Add ? Ctx.numOperands(After) : 1);
  if (!Resources.array(Count * 4, sizeof(SymRef)))
    return Original;
  llvm::SmallVector<SymRef, 128> Left(Ctx.operands(Before));
  llvm::SmallVector<SymRef, 128> Right;
  if (Ctx.op(After) == SymOp::Add)
    Right.append(Ctx.operands(After).begin(), Ctx.operands(After).end());
  else
    Right.push_back(After);
  llvm::sort(Left);
  llvm::sort(Right);
  llvm::SmallVector<SymRef, 128> L, R;
  std::set_difference(Left.begin(), Left.end(), Right.begin(), Right.end(),
                      std::back_inserter(L));
  std::set_difference(Right.begin(), Right.end(), Left.begin(), Left.end(),
                      std::back_inserter(R));
  if (L.empty() || R.empty())
    return Original;
  auto Factors = [&](SymRef Term) {
    auto F = Ctx.operands(Term);
    return Ctx.isConst(F.front()) ? F.drop_front() : F;
  };
  size_t Edges = 0;
  llvm::SmallVector<SymRef, 32> Common, Next;
  for (auto Terms : {llvm::ArrayRef<SymRef>(L), llvm::ArrayRef<SymRef>(R)})
    for (SymRef Term : Terms) {
      if (Ctx.op(Term) != SymOp::Mul)
        return Original;
      auto F = Factors(Term);
      if (F.size() > MaxEdges - Edges)
        return Original;
      if (!Resources.array(F.size() * 3, sizeof(SymRef)))
        return Original;
      if (Edges == 0)
        Common.append(F.begin(), F.end());
      else {
        Next.clear();
        std::set_intersection(Common.begin(), Common.end(), F.begin(), F.end(),
                              std::back_inserter(Next));
        Common.swap(Next);
      }
      Edges += F.size();
      if (Common.size() < 4)
        return Original;
    }
  // Snapshot every quotient before a builder can relocate the operand pool.
  if (!Resources.charge(Edges + Count, 128 * (L.size() + R.size())))
    return Original;
  llvm::SmallVector<llvm::SmallVector<SymRef, 16>, 8> Quotients;
  for (auto Terms : {llvm::ArrayRef<SymRef>(L), llvm::ArrayRef<SymRef>(R)})
    for (SymRef Term : Terms) {
      auto F = Factors(Term);
      Quotients.emplace_back();
      auto &Q = Quotients.back();
      auto First = Ctx.operand(Term, 0);
      if (Ctx.isConst(First))
        Q.push_back(First);
      std::set_difference(F.begin(), F.end(), Common.begin(), Common.end(),
                          std::back_inserter(Q));
    }
  Left.clear();
  Right.clear();
  for (size_t I = 0; I != Quotients.size(); ++I) {
    auto &Q = Quotients[I];
    SymRef Term = Q.empty() ? Ctx.mkOne(Ctx.width(Before)) : Ctx.mkMul(Q);
    (I < L.size() ? Left : Right).push_back(Term);
  }
  return {Ctx.mkAdd(Left), Ctx.mkAdd(Right)};
}

} // namespace

SymRef solveNestedProductCofactors(SymContext &Ctx, SymRef E,
                                   const MBAOptions &Opts, WorkBudget &Budget,
                                   SolveReport &Rep) {
  ArithmeticResources Resources(Budget, Opts.MaxTableBytes);
  auto MarkStopped = llvm::scope_exit([&] {
    Rep.BudgetExhausted |= Resources.stopped() || Budget.exhausted();
  });
  // Admit the syntactic distribution before allocating polynomial tables.
  // Repeated small sums often simplify cheaply in the normal deep walk; an
  // optional completion must not consume that walk's budget by expanding them.
  unsigned Visits = 0;
  bool HasProductSum = false;
  auto FrontierSize = [&](auto &&Self, SymRef R, unsigned Depth,
                          bool ProductOperand) -> unsigned {
    if (++Visits > 2048 || Depth > 32 || !Resources.charge(1))
      return 0;
    const SymOp Op = Ctx.op(R);
    if (Op != SymOp::Add && Op != SymOp::Mul)
      return 1;
    unsigned Terms = Op == SymOp::Add ? 0 : 1;
    for (SymRef Child : Ctx.operands(R)) {
      if (Op == SymOp::Add && ProductOperand && Ctx.op(Child) == SymOp::Mul &&
          Ctx.numOperands(Child) >= 2 &&
          Ctx.numOperands(Child) -
                  unsigned(Ctx.isConst(Ctx.operand(Child, 0))) >=
              2)
        HasProductSum = true;
      unsigned N = Self(Self, Child, Depth + 1, Op == SymOp::Mul);
      if (!N || (Op == SymOp::Add ? N > 128 - Terms : N > 128 / Terms))
        return 0;
      Terms = Op == SymOp::Add ? Terms + N : Terms * N;
    }
    return Terms;
  };
  if (!FrontierSize(FrontierSize, E, 0, false) || !HasProductSum)
    return E;
  ArithmeticNormalizer Normalizer(Ctx, Resources, Ctx.width(E), false);
  auto Original = Normalizer.normalize(E);
  if (!Original || Original->size() > 128)
    return E;
  auto HigherAtoms = [&](const Polynomial &P, std::set<uint32_t> &Atoms) {
    size_t Edges = 0, Higher = 0;
    for (const auto &[Key, Coefficient] : P) {
      size_t Degree = 0;
      for (const auto &[Id, Power] : Key) {
        if (Power > 2048 - Edges || !Resources.charge(1, 128))
          return size_t(0);
        Edges += Power;
        Degree += Power;
      }
      if (Degree >= 4) {
        ++Higher;
        for (const auto &[Id, Power] : Key)
          Atoms.insert(Id);
      }
    }
    return Higher;
  };
  std::set<uint32_t> Atoms;
  if (HigherAtoms(*Original, Atoms) < 2)
    return E;

  // A previous factorization can collapse some partition axes but leave
  // others split. Restore only an existing AND view of a whole product word:
  // W = (W & M) + (W & ~M), exactly at every bit width. Bitwise consumers are
  // opaque here, so this cannot recursively split the word inside its view.
  std::map<uint32_t, SymRef> Splits;
  for (uint32_t Id : Atoms) {
    SymRef View(Id);
    if (Ctx.op(View) != SymOp::And || Ctx.numOperands(View) < 2 ||
        Ctx.numOperands(View) > 16)
      continue;
    if (!Resources.array(Ctx.numOperands(View), sizeof(SymRef)))
      return E;
    llvm::SmallVector<SymRef, 8> Inputs(Ctx.operands(View));
    for (SymRef Word : Inputs) {
      if (Splits.size() == 6 || !Atoms.contains(Word.index()) ||
          Splits.contains(Word.index()))
        continue;
      if (!Resources.array(Inputs.size(), 128 + 4 * sizeof(SymRef)))
        return E;
      llvm::SmallVector<SymRef, 8> Mask;
      for (SymRef Input : Inputs)
        if (Input != Word)
          Mask.push_back(Input);
      SymRef Other = Ctx.mkAnd(Word, Ctx.mkNot(Ctx.mkAnd(Mask)));
      Splits.emplace(Word.index(), Ctx.mkAdd(View, Other));
    }
  }
  llvm::SmallVector<SymRef, 16> Terms, OriginalTerms;
  for (const auto &[Key, Coefficient] : *Original) {
    llvm::SmallVector<SymRef, 16> Factors{Ctx.mkConst(Coefficient)};
    llvm::SmallVector<SymRef, 16> OriginalFactors(Factors);
    for (const auto &[Id, Power] : Key) {
      if (!Resources.array(Power, 4 * sizeof(SymRef)))
        return E;
      auto It = Splits.find(Id);
      Factors.append(Power, It == Splits.end() ? SymRef(Id) : It->second);
      OriginalFactors.append(Power, SymRef(Id));
    }
    Terms.push_back(Ctx.mkMul(Factors));
    OriginalTerms.push_back(Ctx.mkMul(OriginalFactors));
  }
  // Check coefficient and multiplicity emission independently, then apply the
  // exact word partitions and check the expanded spelling as well. Neither
  // this larger intermediate nor an incomplete completion is ever published.
  auto OriginalCheck = Normalizer.normalize(Ctx.mkAdd(OriginalTerms));
  if (!OriginalCheck || *OriginalCheck != *Original)
    return E;
  auto Distributed = Normalizer.normalize(Ctx.mkAdd(Terms));
  Atoms.clear();
  if (!Distributed || Distributed->size() > 128 ||
      HigherAtoms(*Distributed, Atoms) < 6)
    return E;
  auto Expanded = Normalizer.expanded(*Distributed);
  if (!Expanded || *Expanded == E)
    return E;
  auto Check = Normalizer.normalize(*Expanded);
  if (!Check || *Check != *Distributed)
    return E;
  MBAOptions Inner = Opts;
  Inner.MaxTableBytes = Resources.remainingBytes();
  SolveReport Local;
  SymRef Candidate =
      solveProductCofactors(Ctx, *Expanded, Inner, Budget, Local);
  Rep.BudgetExhausted |= Local.BudgetExhausted || Resources.stopped();
  if (Candidate == *Expanded || Local.Evidence != MBAEvidence::Derivation ||
      readingScore(Ctx, Candidate) >= readingScore(Ctx, E))
    return E;
  Rep = Local;
  return Candidate;
}

bool proveArithmeticIdentity(SymContext &Ctx, SymRef Before, SymRef After,
                             WorkBudget &Budget, size_t MaxBytes,
                             bool ExpandComplements) {
  if (!Before.isValid() || !After.isValid() ||
      Ctx.width(Before) != Ctx.width(After))
    return false;
  ArithmeticResources Resources(Budget, MaxBytes);
  if (!ExpandComplements)
    std::tie(Before, After) =
        reduceDistributionProof(Ctx, Before, After, Resources);
  ArithmeticNormalizer Normalizer(Ctx, Resources, Ctx.width(Before),
                                  ExpandComplements);
  auto Left = Normalizer.normalize(Before);
  if (!Left)
    return false;
  auto Right = Normalizer.normalize(After);
  if (!Right || Left->size() != Right->size())
    return false;
  const size_t Words = (size_t(Ctx.width(Before)) + 63) / 64;
  auto R = Right->begin();
  for (const auto &[Key, Coefficient] : *Left) {
    if (!Resources.charge(1 + Key.size() + Words) || Key != R->first ||
        Coefficient != R->second)
      return false;
    ++R;
  }
  return true;
}

SymRef solveArithmetic(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                       WorkBudget &Budget, SolveReport &Rep,
                       bool ExpandComplements) {
  return mayImproveArithmetic(Ctx, E)
             ? normalizeArithmetic(Ctx, E, Opts, Budget, Rep, ExpandComplements)
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
