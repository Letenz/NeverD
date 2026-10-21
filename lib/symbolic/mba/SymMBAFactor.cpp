//===- SymMBAFactor.cpp - Recovering complete shared factors -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SymMBADetail.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace neverd::symbolic::detail {
namespace {

bool isBitwise(const SymContext &Ctx, SymRef R) {
  SymOp Op = Ctx.op(R);
  return Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor ||
         Op == SymOp::Not;
}

// The optional quotient search needs interaction between its terms. A sum of
// shallow, already canonical bitwise terms on disjoint free variables has no
// shared input for that search to eliminate. Keep this eligibility check local:
// arithmetic, opaque nodes, complements, and larger frontiers use the solver.
bool hasIndependentTerms(const SymContext &Ctx, SymRef E) {
  if (Ctx.width(E) == 1 || Ctx.op(E) != SymOp::Add)
    return false;
  std::array<uint32_t, 64> Inputs;
  size_t Count = 0;
  auto AddInput = [&](SymRef R) {
    if (!Ctx.isVar(R) || Count == Inputs.size())
      return false;
    Inputs[Count++] = R.index();
    return true;
  };
  for (SymRef Term : Ctx.operands(E)) {
    if (Ctx.isConst(Term))
      continue;
    if (Ctx.isVar(Term)) {
      if (!AddInput(Term))
        return false;
      continue;
    }
    SymOp Op = Ctx.op(Term);
    if (Op != SymOp::And && Op != SymOp::Or && Op != SymOp::Xor)
      return false;
    for (SymRef Input : Ctx.operands(Term))
      if (!AddInput(Input))
        return false;
  }
  std::sort(Inputs.begin(), Inputs.begin() + Count);
  return std::adjacent_find(Inputs.begin(), Inputs.begin() + Count) ==
         Inputs.begin() + Count;
}

struct FactorUses {
  unsigned Count = 0;
  bool BitwiseQuotient = false;
};

} // namespace

SymRef solveProductCofactors(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                             WorkBudget &Budget, SolveReport &Rep) {
  // Bound this optional frontier before allocating or distributing enclosing
  // scales. Low-degree regions keep their existing reconstruction path.
  constexpr size_t MaxTerms = 128, MaxEdges = 2048, MinCompletionTerms = 6;
  std::array<SymRef, MaxTerms> Pending;
  size_t Top = 1, TermsSeen = 0, Edges = 0, Visits = 0;
  unsigned Higher = 0;
  bool NestedSum = false;
  Pending[0] = E;
  while (Top) {
    SymRef R = Pending[--Top];
    if (++Visits > MaxEdges)
      return E;
    if (Ctx.op(R) == SymOp::Mul && Ctx.numOperands(R) == 2 &&
        Ctx.isConst(Ctx.operand(R, 0)))
      R = Ctx.operand(R, 1);
    if (Ctx.op(R) == SymOp::Add) {
      if (Ctx.numOperands(R) > Pending.size() - Top)
        return E;
      for (SymRef Term : Ctx.operands(R))
        Pending[Top++] = Term;
      continue;
    }
    if (++TermsSeen > MaxTerms)
      return E;
    if (Ctx.op(R) != SymOp::Mul)
      continue;
    auto Factors = Ctx.operands(R);
    if (Ctx.isConst(Factors.front()))
      Factors = Factors.drop_front();
    if (Factors.size() > MaxEdges - Edges)
      return E;
    Edges += Factors.size();
    Higher += Factors.size() >= 4;
    NestedSum |= llvm::any_of(
        Factors, [&](SymRef F) { return Ctx.op(F) == SymOp::Add; });
  }
  if (Budget.exhausted())
    return E;
  if (Higher < 2)
    return NestedSum ? solveNestedProductCofactors(Ctx, E, Opts, Budget, Rep)
                     : E;

  size_t Storage = Opts.MaxTableBytes;
  const size_t WordBytes =
      ((size_t(Ctx.width(E)) - 1) / 64 + 1) * sizeof(uint64_t);
  auto Charge = [&](size_t Work, size_t Slots = 0) {
    const size_t SlotBytes = 128 + 4 * WordBytes;
    if (Slots > Storage / SlotBytes || !Budget.consume(Work)) {
      Rep.BudgetExhausted = true;
      return false;
    }
    Storage -= Slots * SlotBytes;
    return true;
  };
  auto ChargeOperands = [&](SymOp Op, llvm::ArrayRef<SymRef> Operands,
                            size_t Limit) {
    size_t Count = 0;
    for (SymRef R : Operands) {
      const size_t N = Ctx.op(R) == Op ? Ctx.numOperands(R) : 1;
      if (N > Limit - Count) {
        Rep.BudgetExhausted = true;
        return false;
      }
      Count += N;
    }
    return Charge(Count, Count);
  };
  if (!Charge(Visits + Edges, 2 * (TermsSeen + Edges)))
    return E;
  auto Parsed = splitIntoTerms(Ctx, E);
  if (!Parsed)
    return E;
  auto Terms = std::move(*Parsed);
  auto Write = [&](const PolyTerm &Term) {
    llvm::SmallVector<SymRef, 8> Factors(Term.Factors.begin(),
                                         Term.Factors.end());
    Factors.push_back(Ctx.mkConst(Term.Coeff));
    return Ctx.mkMul(Factors);
  };
  auto Read = [&](SymRef R) {
    PolyTerm Term{llvm::APInt(Ctx.width(E), 1), {}};
    if (Ctx.isConst(R)) {
      Term.Coeff = Ctx.constValue(R);
    } else if (Ctx.op(R) == SymOp::Mul) {
      auto Factors = Ctx.operands(R);
      if (Ctx.isConst(Factors.front())) {
        Term.Coeff = Ctx.constValue(Factors.front());
        Factors = Factors.drop_front();
      }
      Term.Factors.append(Factors.begin(), Factors.end());
    } else {
      Term.Factors.push_back(R);
    }
    return Term;
  };
  SymRef Best = E;
  SymRef Current = E;
  std::map<uint32_t, std::pair<SymRef, unsigned>> QuotientCache;
  unsigned Omitted = 1;
  std::array<bool, 2> TriedCompletion{};
  while (!Budget.exhausted()) {
    if (llvm::count_if(
            Terms, [](const PolyTerm &T) { return T.Factors.size() >= 4; }) < 2)
      break;
    // Try linear quotients first, then quadratic ones only when a pass makes
    // no progress. Neither requires expanding the shared higher product.
    std::map<Monomial, llvm::SmallVector<unsigned, 2>> Groups;
    struct Omission {
      unsigned Term, First, Second;
      llvm::SmallVector<unsigned, 2> Indices;
    };
    llvm::SmallVector<Omission, 16> Omissions;
    std::map<uint64_t, llvm::SmallVector<unsigned, 1>> Buckets;
    auto Hash = [](uint32_t Id) {
      uint64_t H = uint64_t(Id) + 0x9e3779b97f4a7c15ULL;
      H = (H ^ (H >> 30)) * 0xbf58476d1ce4e5b9ULL;
      H = (H ^ (H >> 27)) * 0x94d049bb133111ebULL;
      return H ^ (H >> 31);
    };
    bool Complete = true;
    // After completion only a few long residuals may remain. Intersect
    // term pairs instead of enumerating every one/two-factor omission of each
    // term. The shared keys and their lexicographic order are unchanged.
    const bool Pairwise =
        Terms.size() <= 16 && llvm::any_of(Terms, [](const PolyTerm &T) {
          return T.Factors.size() >= 8;
        });
    if (Pairwise) {
      for (unsigned A = 0; A != Terms.size() && Complete; ++A)
        for (unsigned B = A + 1; B != Terms.size() && Complete; ++B) {
          const auto &Left = Terms[A].Factors, &Right = Terms[B].Factors;
          if (!Charge(1)) {
            Complete = false;
            break;
          }
          if (Left.size() < 4 || Left.size() != Right.size())
            continue;
          if (!Charge(Left.size() + Right.size(), Left.size())) {
            Complete = false;
            break;
          }
          llvm::SmallVector<SymRef, 16> Common;
          std::set_intersection(Left.begin(), Left.end(), Right.begin(),
                                Right.end(), std::back_inserter(Common));
          const size_t Required = Left.size() - Omitted;
          if (Common.size() < Required)
            continue;
          const size_t Extra = Common.size() - Required;
          auto Insert = [&](size_t I, size_t J) {
            if (!Charge(Common.size() + 2, Required + 4))
              return false;
            Monomial Key;
            for (size_t K = 0; K != Common.size(); ++K)
              if (K != I && K != J)
                Key.push_back(Common[K].index());
            auto &Indices = Groups[std::move(Key)];
            Indices.push_back(A);
            Indices.push_back(B);
            return true;
          };
          if (Extra == 0) {
            Complete = Insert(Common.size(), Common.size());
          } else {
            for (size_t I = 0; I != Common.size() && Complete; ++I) {
              if (I && Common[I] == Common[I - 1])
                continue;
              if (Extra == 1) {
                Complete = Insert(I, Common.size());
                continue;
              }
              for (size_t J = I + 1; J != Common.size() && Complete; ++J) {
                if (J > I + 1 && Common[J] == Common[J - 1])
                  continue;
                Complete = Insert(I, J);
              }
            }
          }
        }
      for (auto &[Key, Indices] : Groups) {
        if (!Complete || !Charge(Indices.size())) {
          Complete = false;
          break;
        }
        llvm::sort(Indices);
        Indices.erase(std::unique(Indices.begin(), Indices.end()),
                      Indices.end());
      }
    } else {
      for (unsigned T = 0; T != Terms.size() && Complete; ++T) {
        const auto &Factors = Terms[T].Factors;
        if (Factors.size() < 4)
          continue;
        // Short keys cost less to copy than to fingerprint and compare again.
        if (Factors.size() < 12) {
          for (unsigned I = 0; I != Factors.size() && Complete; ++I) {
            if (I && Factors[I] == Factors[I - 1])
              continue;
            for (unsigned J = Omitted == 1 ? I : I + 1; J != Factors.size();
                 ++J) {
              if (J > I + 1 && Factors[J] == Factors[J - 1])
                continue;
              if (!Charge(Factors.size(), Factors.size() + 2)) {
                Complete = false;
                break;
              }
              Monomial Key;
              for (unsigned K = 0; K != Factors.size(); ++K)
                if (K != I && (Omitted == 1 || K != J))
                  Key.push_back(Factors[K].index());
              Groups[std::move(Key)].push_back(T);
              if (Omitted == 1)
                break;
            }
          }
          continue;
        }
        if (!Charge(3 * Factors.size(), Factors.size())) {
          Complete = false;
          break;
        }
        llvm::SmallVector<uint64_t, 16> Hashes;
        uint64_t Fingerprint = 0;
        for (SymRef Factor : Factors) {
          Hashes.push_back(Hash(Factor.index()));
          Fingerprint += Hashes.back();
        }
        for (unsigned I = 0; I != Factors.size(); ++I) {
          if (I && Factors[I] == Factors[I - 1])
            continue;
          for (unsigned J = Omitted == 1 ? I : I + 1; J != Factors.size();
               ++J) {
            if (J > I + 1 && Factors[J] == Factors[J - 1])
              continue;
            if (!Charge(3, 4)) {
              Complete = false;
              break;
            }
            const auto Key =
                Fingerprint - Hashes[I] - (Omitted == 2 ? Hashes[J] : 0);
            auto &Bucket = Buckets[Key];
            unsigned Match = Omissions.size();
            for (unsigned Index : Bucket) {
              const auto &Previous = Omissions[Index];
              const auto &Other = Terms[Previous.Term].Factors;
              if (!Charge(1)) {
                Complete = false;
                break;
              }
              if (Factors.size() != Other.size())
                continue;
              // Fingerprints only select comparisons. Compare the entire
              // retained multiset before sharing a group, even on a collision.
              if (!Charge(Factors.size())) {
                Complete = false;
                break;
              }
              unsigned A = 0, B = 0;
              bool Equal = true;
              while (A != Factors.size()) {
                if (A == I || (Omitted == 2 && A == J)) {
                  ++A;
                  continue;
                }
                while (B == Previous.First ||
                       (Omitted == 2 && B == Previous.Second))
                  ++B;
                if (Factors[A++] != Other[B++]) {
                  Equal = false;
                  break;
                }
              }
              if (Equal) {
                Match = Index;
                break;
              }
            }
            if (!Complete)
              break;
            if (Match == Omissions.size()) {
              Bucket.push_back(Match);
              Omissions.push_back({T, I, J, {T}});
            } else if (Omissions[Match].Indices.back() != T) {
              Omissions[Match].Indices.push_back(T);
            }
            if (Omitted == 1)
              break;
          }
          if (!Complete)
            break;
        }
      }
      // Singleton omissions cannot supply a relation or a cofactor rewrite.
      // Materialize only shared keys, preserving the former lexicographic
      // order.
      for (auto &Entry : Omissions) {
        if (!Complete)
          break;
        if (Entry.Indices.size() < 2)
          continue;
        const auto &Factors = Terms[Entry.Term].Factors;
        if (!Charge(Factors.size(), Factors.size() + 2)) {
          Complete = false;
          break;
        }
        Monomial Key;
        for (unsigned K = 0; K != Factors.size(); ++K)
          if (K != Entry.First && (Omitted == 1 || K != Entry.Second))
            Key.push_back(Factors[K].index());
        Groups.emplace(std::move(Key), std::move(Entry.Indices));
      }
    }
    // Reserve the complete reconstruction before any group can commit.
    if (!Complete || !Charge(Terms.size() + Edges, Terms.size() + Edges))
      break;

    // Reconstruct a complete product in a proved local factor basis, keeping
    // every missing or disturbed coefficient as an exact residual. Completing
    // the whole Cartesian block avoids committing a temporarily larger partial
    // transport when several independent partitions contain the same hole.
    // Keep small frontiers on the cheaper local cofactor path.
    if (!TriedCompletion[Omitted - 1] &&
        llvm::count_if(Terms, [](const PolyTerm &T) {
          return T.Factors.size() >= 4;
        }) >= MinCompletionTerms) {
      TriedCompletion[Omitted - 1] = true;
      struct Relation {
        SymRef Sum, Reduced;
        std::array<llvm::SmallVector<SymRef, 2>, 2> Pieces;
      };
      llvm::SmallVector<Relation, 8> Relations;
      std::set<uint32_t> Seen;
      for (const auto &[Key, Indices] : Groups) {
        if (Indices.size() != 2 || Budget.exhausted())
          continue;
        if (!Charge(2 * (Key.size() + Omitted + 1),
                    2 * (Key.size() + Omitted + 3)))
          break;
        Relation Rel;
        for (unsigned I = 0; I != 2; ++I) {
          size_t K = 0;
          for (SymRef Factor : Terms[Indices[I]].Factors)
            if (K != Key.size() && Factor.index() == Key[K])
              ++K;
            else
              Rel.Pieces[I].push_back(Factor);
          assert(K == Key.size() && Rel.Pieces[I].size() == Omitted &&
                 "completion relation must retain exact multiplicities");
        }
        auto Left = Ctx.mkMul(Rel.Pieces[0]), Right = Ctx.mkMul(Rel.Pieces[1]);
        if (!ChargeOperands(SymOp::Add, {Left, Right}, MaxTerms))
          break;
        Rel.Sum = Ctx.mkAdd(Left, Right);
        if (!Seen.insert(Rel.Sum.index()).second)
          continue;
        if (auto It = QuotientCache.find(Rel.Sum.index());
            It != QuotientCache.end())
          Rel.Reduced = It->second.first;
        else {
          SolveReport Local;
          MBAOptions Inner = Opts;
          Inner.MaxTableBytes = Storage;
          Rel.Reduced = solveOneRegion(Ctx, Rel.Sum, Inner, Budget, Local);
          Rep.BudgetExhausted |= Local.BudgetExhausted;
          if (Rel.Reduced == Rel.Sum ||
              Local.Evidence != MBAEvidence::Derivation)
            continue;
          QuotientCache.try_emplace(Rel.Sum.index(), Rel.Reduced,
                                    Local.NumAtoms);
          Rep.NumAtoms = std::max(Rep.NumAtoms, Local.NumAtoms);
        }
        Relations.push_back(std::move(Rel));
      }
      std::map<uint32_t, llvm::SmallVector<std::pair<unsigned, unsigned>, 2>>
          Heads;
      for (unsigned R = 0; R != Relations.size(); ++R)
        for (unsigned Side = 0; Side != 2; ++Side) {
          if (!Charge(1, 2))
            break;
          Heads[Relations[R].Pieces[Side].front().index()].emplace_back(R,
                                                                        Side);
        }
      SymRef Completion;
      Seen.clear();
      std::map<unsigned, std::pair<llvm::APInt, unsigned>> Scales;
      struct Frequency {
        unsigned Degree;
        llvm::APInt Coefficient;
        unsigned Count;
      };
      llvm::SmallVector<Frequency, 8> Frequencies;
      for (const auto &Term : Terms) {
        const unsigned Degree = Term.Factors.size();
        if (Degree < 4)
          continue;
        bool Counted = false;
        for (auto &Entry : Frequencies) {
          if (!Charge(1 + WordBytes / 8))
            return Best;
          if (Entry.Degree == Degree && Entry.Coefficient == Term.Coeff) {
            ++Entry.Count;
            Counted = true;
            break;
          }
        }
        if (!Counted) {
          if (!Charge(1, 2))
            return Best;
          Frequencies.push_back({Degree, Term.Coeff, 1});
        }
      }
      // Count each distinct scale once. Recounting the complete frontier for
      // every term made a uniform 64-product expansion pay quadratically.
      for (const auto &Entry : Frequencies) {
        if (!Charge(1 + WordBytes / 8, 2))
          return Best;
        auto It = Scales.find(Entry.Degree);
        if (It == Scales.end() || Entry.Count > It->second.second)
          Scales.insert_or_assign(
              Entry.Degree, std::make_pair(Entry.Coefficient, Entry.Count));
      }
      for (const auto &Seed : Terms) {
        if (Seed.Factors.size() < 4 || Relations.size() < 2 ||
            Budget.exhausted() || !Scales.count(Seed.Factors.size()))
          continue;
        const llvm::APInt *Scale = &Scales.at(Seed.Factors.size()).first;
        std::vector<bool> Used(Seed.Factors.size());
        llvm::SmallVector<SymRef, 16> Folded, Expanded;
        llvm::SmallVector<std::pair<const Relation *, SymRef>, 16> Steps;
        bool Fits = true;
        unsigned Axes = 0;
        for (unsigned I = 0; I != Seed.Factors.size() && Fits; ++I) {
          if (Used[I])
            continue;
          const Relation *Chosen = nullptr;
          llvm::SmallVector<unsigned, 2> Positions;
          if (!Charge(1)) {
            Fits = false;
            break;
          }
          if (auto It = Heads.find(Seed.Factors[I].index()); It != Heads.end())
            for (auto [R, Side] : It->second) {
              const auto &Rel = Relations[R];
              const auto &Piece = Rel.Pieces[Side];
              Positions.clear();
              size_t K = 0;
              for (unsigned J = I;
                   J != Seed.Factors.size() && K != Piece.size(); ++J) {
                if (!Charge(1)) {
                  Fits = false;
                  break;
                }
                if (!Used[J] && Seed.Factors[J] == Piece[K]) {
                  Positions.push_back(J);
                  ++K;
                }
              }
              if (K == Piece.size()) {
                Chosen = &Rel;
                break;
              }
              if (!Fits)
                break;
            }
          if (!Fits)
            break;
          Steps.emplace_back(Chosen, Seed.Factors[I]);
          if (Chosen) {
            for (unsigned J : Positions)
              Used[J] = true;
            Folded.push_back(Chosen->Reduced);
            ++Axes;
          } else {
            Used[I] = true;
            Folded.push_back(Seed.Factors[I]);
          }
        }
        // Every relation doubles the Cartesian expansion. Refuse a complete
        // unaffordable plan before copying prefixes that cannot be checked.
        if (!Fits || Axes < 2 || Axes >= std::numeric_limits<size_t>::digits ||
            (size_t(1) << Axes) > MaxTerms ||
            !ChargeOperands(SymOp::Mul, Folded, MaxEdges))
          continue;
        Folded.push_back(Ctx.mkConst(*Scale));
        SymRef Product = Ctx.mkMul(Folded);
        if (!Seen.insert(Product.index()).second)
          continue;
        // Duplicate folded products need no distribution spelling.
        for (const auto &Step : Steps)
          Expanded.push_back(Step.first ? Step.first->Sum : Step.second);
        if (!ChargeOperands(SymOp::Mul, Expanded, MaxEdges))
          continue;
        Expanded.push_back(Ctx.mkConst(*Scale));
        Product = foldProductParity(Ctx, Product, Budget, Storage);
        std::map<Monomial, llvm::APInt> Differences;
        auto Accumulate = [&](Monomial Key, const llvm::APInt &Coefficient) {
          if (!Charge(Key.size() + WordBytes / 8 + 1, Key.size() + 2))
            return false;
          llvm::sort(Key);
          auto It =
              Differences
                  .try_emplace(std::move(Key), llvm::APInt(Ctx.width(E), 0))
                  .first;
          It->second += Coefficient;
          return true;
        };
        for (const auto &Term : Terms) {
          Monomial Key;
          for (SymRef F : Term.Factors)
            Key.push_back(F.index());
          Fits &= Accumulate(std::move(Key), Term.Coeff);
          if (!Fits)
            break;
        }
        // Materialize each complete corner once. Growing every prefix would
        // repeatedly copy unchanged factors, especially in long products with
        // only a few varying axes. The corner bits retain all multiplicities.
        for (size_t Corner = 0; Fits && Corner != (size_t(1) << Axes);
             ++Corner) {
          if (!Charge(Steps.size() + Seed.Factors.size(),
                      Seed.Factors.size())) {
            Fits = false;
            break;
          }
          Monomial Key;
          Key.reserve(Seed.Factors.size());
          unsigned Axis = 0;
          for (const auto &Step : Steps) {
            if (Step.first) {
              for (SymRef F : Step.first->Pieces[(Corner >> Axis) & 1])
                Key.push_back(F.index());
              ++Axis;
            } else {
              Key.push_back(Step.second.index());
            }
          }
          Fits = Accumulate(std::move(Key), -*Scale);
        }
        if (!Fits)
          break;
        llvm::SmallVector<SymRef, 16> Residuals;
        for (const auto &[Key, Coefficient] : Differences) {
          if (Coefficient.isZero())
            continue;
          PolyTerm Term{Coefficient, {}};
          for (auto Id : Key)
            Term.Factors.push_back(SymRef(Id));
          Residuals.push_back(Write(Term));
        }
        llvm::SmallVector<SymRef, 16> DistributionResiduals(Residuals);
        // Complete the original distribution first. Parity projection is an
        // independent exact step and must not erase factors from that proof.
        for (SymRef &Residual : Residuals)
          Residual = foldProductParity(Ctx, Residual, Budget, Storage);
        Residuals.push_back(Product);
        if (!ChargeOperands(SymOp::Add, Residuals, MaxTerms))
          continue;
        SymRef Candidate = Ctx.mkAdd(Residuals);
        if (readingScore(Ctx, Candidate) >= readingScore(Ctx, Current) ||
            (Completion.isValid() &&
             readingScore(Ctx, Candidate) >= readingScore(Ctx, Completion)))
          continue;
        DistributionResiduals.push_back(Ctx.mkMul(Expanded));
        if (!ChargeOperands(SymOp::Add, DistributionResiduals, MaxTerms))
          continue;
        if (proveArithmeticIdentity(Ctx, Current,
                                    Ctx.mkAdd(DistributionResiduals), Budget,
                                    Storage, /*ExpandComplements=*/false))
          Completion = Candidate;
      }
      if (Completion.isValid()) {
        Current = Completion;
        if (readingScore(Ctx, Current) < readingScore(Ctx, Best))
          Best = Current;
        Rep.Outcome = MBAOutcome::Rewritten;
        Rep.Evidence = MBAEvidence::Derivation;
        auto CompletedTerms = splitIntoTerms(Ctx, Current);
        if (!CompletedTerms)
          break;
        Terms = std::move(*CompletedTerms);
        Omitted = 1;
        continue;
      }
    }
    std::vector<bool> Used(Terms.size(), false);
    llvm::SmallVector<PolyTerm, 8> Next;
    llvm::SmallVector<SymRef, 16> Distribution;
    for (const auto &[Key, Indices] : Groups) {
      if (Budget.exhausted())
        break;
      if (llvm::count_if(Indices, [&](unsigned T) { return !Used[T]; }) < 2)
        continue;
      if (!Charge(Indices.size() * (Key.size() + 2),
                  Indices.size() * (Key.size() + 4)))
        break;
      llvm::SmallVector<SymRef, 8> Common, Quotients, Originals;
      for (uint32_t Id : Key)
        Common.push_back(SymRef(Id));
      auto First = llvm::find_if(Indices, [&](unsigned T) { return !Used[T]; });
      const auto &Scale = Terms[*First].Coeff;
      const bool SameScale = llvm::all_of(Indices, [&](unsigned T) {
        return Used[T] || Terms[T].Coeff == Scale;
      });
      if (SameScale)
        Common.push_back(Ctx.mkConst(Scale));
      for (unsigned T : Indices) {
        if (Used[T])
          continue;
        const auto &Term = Terms[T];
        llvm::SmallVector<SymRef, 8> Remaining;
        size_t K = 0;
        for (SymRef Factor : Term.Factors)
          if (K != Key.size() && Factor.index() == Key[K])
            ++K;
          else
            Remaining.push_back(Factor);
        assert(K == Key.size() && "cofactor must retain exact multiplicities");
        if (!SameScale)
          Remaining.push_back(Ctx.mkConst(Term.Coeff));
        Quotients.push_back(Ctx.mkMul(Remaining));
        Originals.push_back(Write(Term));
      }
      if (!ChargeOperands(SymOp::Add, Quotients, MaxTerms) ||
          !ChargeOperands(SymOp::Add, Originals, MaxTerms) ||
          !ChargeOperands(SymOp::Mul, Common, MaxEdges))
        break;
      const SymRef WeightedQuotient = Ctx.mkAdd(Quotients);
      SymRef Quotient = WeightedQuotient;
      SolveReport Local;
      SymRef Reduced = Quotient;
      auto Solve = [&] {
        Local = {};
        Reduced = Quotient;
        if (!Charge(1, 2))
          return false;
        if (auto It = QuotientCache.find(Quotient.index());
            It != QuotientCache.end()) {
          Reduced = It->second.first;
          Local.NumAtoms = It->second.second;
          Local.Evidence = MBAEvidence::Derivation;
        } else {
          MBAOptions Inner = Opts;
          Inner.MaxTableBytes = Storage;
          Reduced = solveOneRegion(Ctx, Quotient, Inner, Budget, Local);
          Rep.BudgetExhausted |= Local.BudgetExhausted;
          if (Reduced != Quotient && Local.Evidence == MBAEvidence::Derivation)
            QuotientCache.try_emplace(Quotient.index(), Reduced,
                                      Local.NumAtoms);
        }
        return Reduced != Quotient && Local.Evidence == MBAEvidence::Derivation;
      };
      llvm::SmallVector<PolyTerm, 4> Residuals;
      llvm::SmallVector<SymRef, 4> ResidualExprs;
      if (!SameScale) {
        // A changed coefficient need not hide the common identity. Write
        // sum(c_i*q_i) = k*sum(q_i) + sum((c_i-k)*q_i), retaining every
        // difference modulo the word width. No coefficient is divided.
        // Prefer a scale repeated elsewhere at this degree, so successive
        // groups can expose the same product even across a sparse disturbance.
        if (!Charge(Indices.size() * Terms.size() * (WordBytes / 8),
                    Indices.size() * (Key.size() + Omitted + 4)))
          break;
        unsigned ScaleIndex = *First, Frequency = 0;
        for (unsigned T : Indices) {
          if (Used[T])
            continue;
          unsigned Count = llvm::count_if(Terms, [&](const PolyTerm &Term) {
            return Term.Factors.size() == Terms[T].Factors.size() &&
                   Term.Coeff == Terms[T].Coeff;
          });
          if (Count > Frequency) {
            Frequency = Count;
            ScaleIndex = T;
          }
        }
        const auto &CommonScale = Terms[ScaleIndex].Coeff;
        Common.push_back(Ctx.mkConst(CommonScale));
        Quotients.clear();
        for (unsigned T : Indices) {
          if (Used[T])
            continue;
          const auto &Term = Terms[T];
          llvm::SmallVector<SymRef, 8> Remaining;
          size_t K = 0;
          for (SymRef Factor : Term.Factors)
            if (K != Key.size() && Factor.index() == Key[K])
              ++K;
            else
              Remaining.push_back(Factor);
          Quotients.push_back(Ctx.mkMul(Remaining));
          auto Difference = Term.Coeff - CommonScale;
          if (!Difference.isZero()) {
            Residuals.push_back({std::move(Difference), Term.Factors});
            ResidualExprs.push_back(Write(Residuals.back()));
          }
        }
        if (!ChargeOperands(SymOp::Add, Quotients, MaxTerms) ||
            !ChargeOperands(SymOp::Mul, Common, MaxEdges))
          break;
        Quotient = Ctx.mkAdd(Quotients);
      }
      auto Accept = [&] {
        SymRef Original = Ctx.mkAdd(Originals);
        SymRef Cofactor = Ctx.mkMul(Common);
        if (!ChargeOperands(SymOp::Mul, {Cofactor, Reduced}, MaxEdges) ||
            !ChargeOperands(SymOp::Mul, {Cofactor, Quotient}, MaxEdges))
          return false;
        SymRef Candidate = Ctx.mkMul(Cofactor, Reduced);
        llvm::SmallVector<SymRef, 4> Trial(ResidualExprs);
        Trial.push_back(Candidate);
        if (!ChargeOperands(SymOp::Add, Trial, MaxTerms) ||
            readingScore(Ctx, Ctx.mkAdd(Trial)) >= readingScore(Ctx, Original))
          return false;
        Next.push_back(Read(Candidate));
        Next.append(Residuals.begin(), Residuals.end());
        Distribution.push_back(Ctx.mkMul(Cofactor, Quotient));
        Distribution.append(ResidualExprs.begin(), ResidualExprs.end());
        for (unsigned T : Indices)
          Used[T] = true;
        Rep.NumAtoms = std::max(Rep.NumAtoms, Local.NumAtoms);
        return true;
      };
      if (Solve() && Accept())
        continue;
      if (!SameScale && !Budget.exhausted()) {
        // If the unit-coefficient identity does not shorten this group,
        // preserve the original weighted quotient's independent opportunity.
        Common.pop_back();
        Residuals.clear();
        ResidualExprs.clear();
        Quotient = WeightedQuotient;
        if (Solve())
          Accept();
      }
    }
    if (Next.empty()) {
      if (Omitted++ == 1 && !Rep.BudgetExhausted)
        continue;
      break;
    }
    Omitted = 1;
    for (unsigned T = 0; T != Terms.size(); ++T)
      if (!Used[T]) {
        Distribution.push_back(Write(Terms[T]));
        Next.push_back(std::move(Terms[T]));
      }
    // Check the complete distribution, including all untouched terms, before
    // committing any quotient substitutions. A single proof shares the
    // normalizer's exact-node memo across groups. The independently proved
    // quotients then compose under their unchanged cofactors.
    if (!ChargeOperands(SymOp::Add, Distribution, MaxTerms) ||
        !proveArithmeticIdentity(Ctx, Current, Ctx.mkAdd(Distribution), Budget,
                                 Storage,
                                 /*ExpandComplements=*/false))
      break;
    Terms = std::move(Next);
    llvm::SmallVector<SymRef, 16> Summands;
    for (const auto &Term : Terms)
      Summands.push_back(Write(Term));
    if (!ChargeOperands(SymOp::Add, Summands, MaxTerms))
      break;
    Current = Ctx.mkAdd(Summands);
    if (readingScore(Ctx, Current) < readingScore(Ctx, Best)) {
      Best = Current;
      Rep.Outcome = MBAOutcome::Rewritten;
      Rep.Evidence = MBAEvidence::Derivation;
    }
  }
  Rep.BudgetExhausted |= Budget.exhausted();
  return Best;
}

SymRef solveStructuralFactors(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                              WorkBudget &Budget, SolveReport &Rep) {
  // This optional reading only inspects immediate sums and products. Purely
  // arithmetic factoring belongs to the polynomial normalizer. A small fixed
  // frontier also keeps unsuccessful scans independent of shared tail depth.
  constexpr size_t MaxTerms = 64, MaxEdges = 256;
  if (Ctx.op(E) != SymOp::Add || Ctx.numOperands(E) < 3 ||
      Ctx.numOperands(E) > MaxTerms || Budget.exhausted())
    return E;
  unsigned Products = 0;
  bool Bitwise = false;
  size_t Edges = 0;
  for (SymRef Term : Ctx.operands(E)) {
    if (Ctx.op(Term) != SymOp::Mul)
      continue;
    llvm::ArrayRef<SymRef> Factors = Ctx.operands(Term);
    if (Factors.size() > MaxEdges - Edges)
      return E;
    Edges += Factors.size();
    Products += Factors.size() - unsigned(Ctx.isConst(Factors.front())) > 1;
    for (SymRef Factor : Factors)
      Bitwise |= isBitwise(Ctx, Factor);
  }
  if (Products < 2 || !Bitwise)
    return E;

  SymRef Best = E;
  size_t Storage = Opts.MaxTableBytes;
  const size_t WordBytes =
      (size_t(Ctx.width(E)) / 64 + (Ctx.width(E) % 64 != 0)) * sizeof(uint64_t);
  const size_t SlotBytes = 128 + 3 * WordBytes;
  auto Charge = [&](size_t Work, size_t Slots) {
    if (Slots > Storage / SlotBytes || !Budget.consume(Work)) {
      Rep.BudgetExhausted = true;
      return false;
    }
    Storage -= Slots * SlotBytes;
    return true;
  };
  auto ChargeOperands = [&](SymOp Op, llvm::ArrayRef<SymRef> Operands,
                            size_t Limit) {
    size_t Arity = 0;
    for (SymRef R : Operands) {
      const size_t N = Ctx.op(R) == Op ? Ctx.numOperands(R) : 1;
      if (N > Limit - Arity) {
        Rep.BudgetExhausted = true;
        return false;
      }
      Arity += N;
    }
    return Charge(Arity, Arity);
  };
  // Counts, the original term snapshot, and scratch operand buffers are all
  // charged before allocation. Each scan below is bounded by this frontier.
  const size_t Count = Ctx.numOperands(E);
  if (!Charge(Count + Edges, Count + Edges))
    return Best;
  llvm::SmallVector<SymRef, 16> Terms(Ctx.operands(E).begin(),
                                      Ctx.operands(E).end());
  std::map<uint32_t, FactorUses> Uses;
  for (SymRef Term : Terms) {
    if (Ctx.isConst(Term))
      continue;
    if (Ctx.op(Term) != SymOp::Mul) {
      ++Uses[Term.index()].Count;
      continue;
    }
    llvm::ArrayRef<SymRef> Factors = Ctx.operands(Term);
    const unsigned BitwiseFactors =
        std::count_if(Factors.begin(), Factors.end(),
                      [&](SymRef F) { return isBitwise(Ctx, F); });
    SymRef Previous;
    // Canonical products sort nonconstant factors. A repeated factor counts
    // once per term, while removing one copy can leave another in the quotient.
    for (SymRef Factor : Factors) {
      if (Ctx.isConst(Factor) || Factor == Previous)
        continue;
      Previous = Factor;
      auto &Use = Uses[Factor.index()];
      ++Use.Count;
      Use.BitwiseQuotient |= BitwiseFactors > unsigned(isBitwise(Ctx, Factor));
    }
  }
  llvm::SmallVector<uint32_t, 2> Choices;
  for (const auto &[Id, Use] : Uses) {
    if (Use.Count < 2 ||
        (!Use.BitwiseQuotient && Ctx.op(SymRef(Id)) != SymOp::Not))
      continue;
    auto Position =
        std::find_if(Choices.begin(), Choices.end(),
                     [&](uint32_t I) { return Use.Count > Uses.at(I).Count; });
    Choices.insert(Position, Id);
    if (Choices.size() > 2)
      Choices.pop_back();
  }

  for (uint32_t Id : Choices) {
    if (!Charge(Count + Edges, Count + Edges + 4))
      break;
    SymRef Factor(Id);
    llvm::SmallVector<SymRef, 16> Quotients, Rest;
    for (SymRef Term : Terms) {
      if (Term == Factor) {
        Quotients.push_back(Ctx.mkOne(Ctx.width(E)));
        continue;
      }
      if (Ctx.op(Term) != SymOp::Mul) {
        Rest.push_back(Term);
        continue;
      }
      llvm::ArrayRef<SymRef> Factors = Ctx.operands(Term);
      auto Found = std::find(Factors.begin(), Factors.end(), Factor);
      if (Found == Factors.end()) {
        Rest.push_back(Term);
        continue;
      }
      llvm::SmallVector<SymRef, 8> Remaining(Factors.begin(), Factors.end());
      Remaining.erase(Remaining.begin() + (Found - Factors.begin()));
      Quotients.push_back(Ctx.mkMul(Remaining));
    }
    // Every original term is placed once, and exactly one identical factor
    // is removed from each selected term. Distributivity in the original word
    // ring therefore proves E = Factor * sum(Quotients) + sum(Rest), without
    // treating related factors as independent or inferring anything from
    // samples.
    // Removing a factor can expose an arbitrarily wide sum. Account for the
    // actual canonical flattening before any builder copies its operands.
    if (!ChargeOperands(SymOp::Add, Quotients, MaxTerms))
      continue;
    // These quotients were created after the deep walk's original frontier.
    // Apply its exact parity projection before searching an unobservable
    // product. Keep the higher-product recovery's original factor basis.
    for (SymRef &Part : Quotients)
      Part = foldProductParity(Ctx, Part, Budget, Storage);
    SymRef Quotient = Ctx.mkAdd(Quotients);
    auto Consider = [&](SymRef Value, unsigned NumAtoms) {
      if (!ChargeOperands(SymOp::Mul, {Factor, Value}, MaxEdges))
        return;
      // Multiplication by one can expose the complete factor as an Add.
      // Check that degeneracy before constructing either candidate node. Rest
      // comes from the canonical original Add and has no direct Add child.
      const size_t Added = Ctx.isConst(Value) &&
                                   Ctx.constValue(Value).isOne() &&
                                   Ctx.op(Factor) == SymOp::Add
                               ? Ctx.numOperands(Factor)
                               : 1;
      if (Added > MaxTerms || Rest.size() > MaxTerms - Added) {
        Rep.BudgetExhausted = true;
        return;
      }
      bool Fits = Charge(Rest.size() + Added, Rest.size() + Added);
      if (!Fits)
        return;
      Rest.push_back(Ctx.mkMul(Factor, Value));
      SymRef Candidate = Ctx.mkAdd(Rest);
      Rest.pop_back();
      if (readingScore(Ctx, Candidate) < readingScore(Ctx, Best)) {
        Best = Candidate;
        Rep.NumAtoms = NumAtoms;
        Rep.Outcome = MBAOutcome::Rewritten;
        Rep.Evidence = MBAEvidence::Derivation;
      }
    };
    // Distributivity already proves this candidate. A failed optional quotient
    // search must not discard a strictly better completed factorization.
    Consider(Quotient, 0);
    if (hasIndependentTerms(Ctx, Quotient))
      continue;
    MBAOptions Inner = Opts;
    Inner.MaxTableBytes = Storage;
    SolveReport Local;
    SymRef Reduced = solveOneRegion(Ctx, Quotient, Inner, Budget, Local);
    Rep.BudgetExhausted |= Local.BudgetExhausted;
    if (Reduced == Quotient || Local.Evidence != MBAEvidence::Derivation)
      continue;
    // solveOneRegion independently proved the quotient replacement. Reusing
    // it under this unchanged factor instantiates that exact identity.
    Consider(Reduced, Local.NumAtoms);
    if (Budget.exhausted())
      break;
  }
  return Best;
}

} // namespace neverd::symbolic::detail
