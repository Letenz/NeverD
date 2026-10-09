//===- ConditionalImplication.cpp - Bounded whole-domain implication
//===========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "ConditionalImplication.h"

#include "ProofNode.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace neverd::analysis::conditional_implication {
namespace {
using symbolic::SymContext;
using symbolic::SymOp;
using symbolic::SymRef;
struct Budget {
  uint64_t Remaining, Used = 0;
  bool Active = true;
  explicit Budget(uint64_t N) : Remaining(N) {}
  bool charge(uint64_t N = 1) {
    if (!Active || N > Remaining) {
      Active = false;
      return false;
    }
    Remaining -= N;
    Used += N;
    return true;
  }
};

// Proof-local canonicalization preserves general execution/query construction.
inline SymRef booleanEquality(SymContext &C, SymRef A, SymRef B,
                              Budget *Work = nullptr) {
  if (C.width(A) != C.width(B))
    return {};
  for (unsigned I = 0; I != 2; ++I) {
    const auto Number = I ? B : A, Other = I ? A : B;
    const unsigned Words = (C.width(Number) - 1) / 64 + 1;
    if (!C.isConst(Number) || Words > 64)
      continue;
    auto Bit = Other;
    if (C.op(Other) == SymOp::ZExt)
      Bit = C.operand(Other, 0);
    if (C.width(Bit) != 1)
      continue;
    if (Work && !Work->charge(3 * Words))
      return C.mkEq(A, B);
    const auto Value = C.constValue(Number);
    if (Value.isOne())
      return Bit;
    if (Value.isZero())
      return C.mkNot(Bit);
  }
  return C.mkEq(A, B);
}

using Literal = std::pair<SymRef, bool>;
inline std::optional<std::vector<Literal>> conjuncts(const SymContext &C,
                                                     SymRef Root, Budget &B) {
  std::vector<Literal> Work{{Root, true}}, Out;
  llvm::DenseSet<uint64_t> Seen;
  while (!Work.empty()) {
    if (!B.charge())
      return std::nullopt;
    const auto [At, Positive] = Work.back();
    Work.pop_back();
    if (!At || At.index() >= C.numNodes() || C.width(At) != 1)
      return std::nullopt;
    if (!Seen.insert((uint64_t(At.index()) << 1) | Positive).second)
      continue;
    const auto Op = C.op(At);
    const auto Args = C.operands(At);
    if ((Op == SymOp::And && Positive) || (Op == SymOp::Or && !Positive)) {
      if (Args.empty() || !B.charge(Args.size()))
        return std::nullopt;
      for (auto A : Args)
        Work.push_back({A, Positive});
    } else if (Op == SymOp::Not) {
      if (Args.size() != 1 || !B.charge())
        return std::nullopt;
      Work.push_back({Args[0], !Positive});
    } else {
      Out.push_back({At, Positive});
    }
  }
  std::sort(Out.begin(), Out.end(), [](auto A, auto B) {
    return A.first != B.first ? A.first < B.first : A.second < B.second;
  });
  return Out;
}

struct Node {
  SymRef Ref;
  SymOp Op;
  unsigned Width;
  uint64_t Aux;
  std::vector<unsigned> Args;
};

struct ConstantKey {
  unsigned Width;
  std::array<uint64_t, 4> Words{};
  auto operator<=>(const ConstantKey &) const = default;
};

struct Signature {
  SymOp Op;
  unsigned Width;
  uint64_t Aux;
  std::vector<unsigned> Args;
  bool operator==(const Signature &) const = default;
};
struct SignatureHash {
  size_t operator()(const Signature &S) const {
    return llvm::hash_combine(
        unsigned(S.Op), S.Width, S.Aux,
        llvm::hash_combine_range(S.Args.begin(), S.Args.end()));
  }
};

class Closure {
public:
  const std::vector<Node> &Nodes;
  Budget Work;
  std::vector<unsigned> Parent, Size, Minimum;
  std::vector<std::optional<llvm::APInt>> Values;
  std::map<ConstantKey, unsigned> Constants;
  uint64_t Unions = 0;
  bool Contradiction = false;

  Closure(const std::vector<Node> &N, uint64_t MaxWork)
      : Nodes(N), Work(MaxWork), Parent(N.size()), Size(N.size(), 1),
        Minimum(N.size()), Values(N.size()) {
    for (unsigned I = 0; I != N.size(); ++I)
      Parent[I] = Minimum[I] = I;
  }
  unsigned find(unsigned I) {
    while (Parent[I] != I) {
      if (!Work.charge())
        return I;
      Parent[I] = Parent[Parent[I]];
      I = Parent[I];
    }
    return I;
  }
  bool merge(unsigned A, unsigned B) {
    if (!Work.charge())
      return false;
    A = find(A);
    B = find(B);
    if (!Work.Active)
      return false;
    if (A == B)
      return true;
    if (Nodes[A].Width != Nodes[B].Width)
      return false;
    if (!Work.charge((Nodes[A].Width + 63) / 64))
      return false;
    if (Values[A] && Values[B] && *Values[A] != *Values[B]) {
      Contradiction = true;
      return true;
    }
    if (Size[A] < Size[B])
      std::swap(A, B);
    Parent[B] = A;
    Size[A] += Size[B];
    Minimum[A] = std::min(Minimum[A], Minimum[B]);
    if (!Values[A] && Values[B])
      Values[A] = Values[B];
    ++Unions;
    return true;
  }
  bool assign(unsigned I, const llvm::APInt &Value) {
    if (Value.getBitWidth() != Nodes[I].Width || Value.getBitWidth() > 256)
      return false;
    const unsigned Words = Value.getNumWords();
    if (!Work.charge(3 * Words + 1))
      return false;
    ConstantKey Key{Value.getBitWidth(), {}};
    std::copy_n(Value.getRawData(), Words, Key.Words.begin());
    I = find(I);
    if (!Work.Active)
      return false;
    if (Values[I] && *Values[I] != Value) {
      Contradiction = true;
      return true;
    }
    auto [It, Added] = Constants.emplace(Key, I);
    if (!Added)
      return merge(I, It->second);
    Values[I] = Value;
    return true;
  }
  std::optional<llvm::APInt> known(unsigned I) {
    I = find(I);
    if (!Work.charge((Nodes[I].Width + 63) / 64))
      return std::nullopt;
    return Values[I];
  }
  void boolean(unsigned I, bool Value) { assign(I, llvm::APInt(1, Value)); }

  void process(unsigned I,
               std::unordered_map<Signature, unsigned, SignatureHash> &Table) {
    const auto &N = Nodes[I];
    if (!Work.charge(1 + 3 * N.Args.size()))
      return;
    if (N.Op == SymOp::Const || N.Op == SymOp::Var)
      return;
    Signature Key{N.Op, N.Width, N.Aux, {}};
    for (auto A : N.Args)
      Key.Args.push_back(find(A));
    if (!Work.Active)
      return;
    if (symbolic::isCommutative(N.Op) || N.Op == SymOp::Eq)
      std::sort(Key.Args.begin(), Key.Args.end());
    auto [It, Added] = Table.emplace(std::move(Key), I);
    if (!Added)
      merge(I, It->second);
    std::vector<std::optional<llvm::APInt>> V;
    for (auto A : N.Args)
      V.push_back(known(A));
    auto Own = known(I);
    if (!Work.Active)
      return;
    if (N.Op == SymOp::Eq) {
      if (find(N.Args[0]) == find(N.Args[1]))
        boolean(I, true);
      else if (V[0] && V[1])
        boolean(I, *V[0] == *V[1]);
      const auto Updated = known(I);
      if (Updated && Updated->isOne())
        merge(N.Args[0], N.Args[1]);
    } else if (N.Op == SymOp::Ult || N.Op == SymOp::Ule || N.Op == SymOp::Slt ||
               N.Op == SymOp::Sle) {
      if (find(N.Args[0]) == find(N.Args[1]))
        boolean(I, N.Op == SymOp::Ule || N.Op == SymOp::Sle);
    } else if (N.Op == SymOp::Not && N.Width == 1) {
      if (V[0])
        boolean(I, V[0]->isZero());
      if (Own)
        boolean(N.Args[0], Own->isZero());
    } else if ((N.Op == SymOp::And || N.Op == SymOp::Or) && N.Width == 1) {
      const bool Annihilator = N.Op == SymOp::Or;
      const auto Is = [](const auto &X, bool B) {
        return X && X->isOne() == B;
      };
      if (std::any_of(V.begin(), V.end(),
                      [&](const auto &X) { return Is(X, Annihilator); }))
        boolean(I, Annihilator);
      else if (std::all_of(V.begin(), V.end(),
                           [&](const auto &X) { return Is(X, !Annihilator); }))
        boolean(I, !Annihilator);
      if (Is(Own, !Annihilator))
        for (auto A : N.Args)
          boolean(A, !Annihilator);
    } else if ((N.Op == SymOp::ZExt || N.Op == SymOp::SExt) &&
               Nodes[N.Args[0]].Width == 1) {
      const llvm::APInt One = N.Op == SymOp::ZExt
                                  ? llvm::APInt(N.Width, 1)
                                  : llvm::APInt::getAllOnes(N.Width);
      if (V[0])
        assign(I, V[0]->isOne() ? One : llvm::APInt(N.Width, 0));
      if (Own) {
        if (!Own->isZero() && *Own != One)
          Contradiction = true;
        else
          boolean(N.Args[0], !Own->isZero());
      }
    }
  }
};

struct Prepared {
  const SymContext *SourceContext = nullptr;
  SymRef SourceDomain, SourceGoal;
  std::vector<SymRef> Original, Images, Constraints, Inputs, Terms;
  SymRef Domain, Goal;
  uint64_t Unions = 0, ClosureWork = 0, PreparationWork = 0;
  uint64_t CollectionWork = 0, RebuildWork = 0, NormalizationWork = 0;
  std::vector<SymRef> NormalizationOriginal, NormalizationImages;
  bool ClosureExhausted = false, DomainContradiction = false;
};

inline std::optional<Prepared> prepareCanonical(SymContext &C, SymRef Domain,
                                                SymRef Goal,
                                                const Limits &L = {}) {
  const size_t OriginalSize = C.numNodes();
  if (!Domain || !Goal || Domain.index() >= OriginalSize ||
      Goal.index() >= OriginalSize || C.width(Domain) != 1 ||
      C.width(Goal) != 1 || OriginalSize > L.MaxNodes || !L.MaxWidth)
    return std::nullopt;
  Budget B(std::min<uint64_t>(L.MaxWork, 4194304));
  const unsigned WidthLimit = std::min(L.MaxWidth, 256U);
  std::vector<SymRef> Work{Domain, Goal}, Refs;
  llvm::DenseSet<uint32_t> Seen;
  while (!Work.empty()) {
    if (!B.charge())
      return std::nullopt;
    auto R = Work.back();
    Work.pop_back();
    if (!R || R.index() >= OriginalSize)
      return std::nullopt;
    if (!Seen.insert(R.index()).second)
      continue;
    if (Seen.size() > std::min(L.MaxReachableNodes, 262144U) || !C.width(R) ||
        C.width(R) > WidthLimit)
      return std::nullopt;
    const auto Args = C.operands(R);
    if (!B.charge(Args.size()) || !detail::validProofNode(C, R))
      return std::nullopt;
    for (auto A : Args) {
      if (!A || A.index() >= R.index())
        return std::nullopt;
      Work.push_back(A);
    }
    Refs.push_back(R);
  }
  std::sort(Refs.begin(), Refs.end());
  llvm::DenseMap<uint32_t, unsigned> Index;
  std::vector<Node> Nodes;
  for (auto R : Refs) {
    if (!B.charge(1 + C.numOperands(R)))
      return std::nullopt;
    const auto N = C.node(R);
    Node Copy{R, N.Op, N.Width, N.Aux, {}};
    for (auto A : C.operands(R))
      Copy.Args.push_back(Index.at(A.index()));
    Index[R.index()] = Nodes.size();
    Nodes.push_back(std::move(Copy));
  }
  auto Facts = conjuncts(C, Domain, B);
  if (!Facts)
    return std::nullopt;
  Closure U(Nodes, std::min<uint64_t>(L.MaxWork, 4194304));
  for (unsigned I = 0; I != Nodes.size(); ++I) {
    if (Nodes[I].Op == SymOp::Const) {
      if (!U.Work.charge((Nodes[I].Width + 63) / 64))
        break;
      U.assign(I, C.constValue(Nodes[I].Ref));
    }
  }
  for (const auto &[R, Positive] : *Facts) {
    U.boolean(Index.at(R.index()), Positive);
    if (Positive && C.op(R) == SymOp::Eq) {
      const auto &N = Nodes[Index.at(R.index())];
      U.merge(N.Args[0], N.Args[1]);
    }
    if (!U.Work.Active || U.Contradiction)
      break;
  }
  for (unsigned Pass = 0; Pass != 16 && U.Work.Active && !U.Contradiction;
       ++Pass) {
    const auto Before = U.Unions;
    std::unordered_map<Signature, unsigned, SignatureHash> Table;
    for (unsigned I = 0; I != Nodes.size() && U.Work.Active && !U.Contradiction;
         ++I)
      U.process(I, Table);
    if (U.Unions == Before)
      break;
  }
  Prepared P;
  P.Original = Refs;
  P.Unions = U.Unions;
  P.ClosureWork = U.Work.Used;
  P.ClosureExhausted = !U.Work.Active;
  P.DomainContradiction = U.Contradiction;
  // A separate bounded read-only walk consumes the already sound partial
  // closure. Exhaustion does not invent further equivalences.
  Budget R(std::min<uint64_t>(L.MaxWork, 4194304));
  for (unsigned I = 0; I != Nodes.size(); ++I) {
    if (!R.charge(1 + Nodes[I].Args.size()))
      return std::nullopt;
    unsigned Root = I;
    while (U.Parent[Root] != Root) {
      if (!R.charge())
        return std::nullopt;
      Root = U.Parent[Root];
    }
    SymRef Image;
    if (U.Values[Root]) {
      if (!R.charge((Nodes[I].Width + 63) / 64))
        return std::nullopt;
      Image = C.mkConst(*U.Values[Root]);
    } else if (U.Minimum[Root] != I) {
      if (U.Minimum[Root] >= I)
        return std::nullopt;
      Image = P.Images[U.Minimum[Root]];
    } else {
      std::vector<SymRef> Args;
      for (auto A : Nodes[I].Args)
        Args.push_back(P.Images[A]);
      Image = C.rebuild(Nodes[I].Ref, Args);
      if (C.op(Image) == SymOp::Eq) {
        const auto A = C.operand(Image, 0), B = C.operand(Image, 1);
        Image = booleanEquality(C, A, B, &R);
      }
      if (!R.Active)
        return std::nullopt;
    }
    if (!Image || C.width(Image) != Nodes[I].Width || C.numNodes() > L.MaxNodes)
      return std::nullopt;
    P.Images.push_back(Image);
    if (Nodes[I].Op == SymOp::Var)
      P.Inputs.push_back(Image);
  }
  for (unsigned I = 0; I != Nodes.size(); ++I) {
    if (!R.charge(1 + Nodes[I].Args.size()))
      return std::nullopt;
    if (Nodes[I].Op == SymOp::Var)
      continue;
    std::vector<SymRef> Args;
    for (auto A : Nodes[I].Args)
      Args.push_back(P.Images[A]);
    auto Rebuilt = C.rebuild(Nodes[I].Ref, Args);
    if (C.op(Rebuilt) == SymOp::Eq) {
      const auto A = C.operand(Rebuilt, 0), B = C.operand(Rebuilt, 1);
      Rebuilt = booleanEquality(C, A, B, &R);
    }
    auto Def = booleanEquality(C, P.Images[I], Rebuilt, &R);
    if (!R.Active)
      return std::nullopt;
    if (C.numNodes() > L.MaxNodes)
      return std::nullopt;
    if (!C.isConst(Def) || !C.constValue(Def).isOne())
      P.Constraints.push_back(Def);
  }
  P.Domain = P.Images[Index.at(Domain.index())];
  P.Goal = P.Images[Index.at(Goal.index())];
  P.Constraints.push_back(P.Domain);
  if (U.Contradiction)
    P.Constraints.push_back(C.mkFalse());
  const auto Full = C.mkAnd(P.Constraints);
  if (C.numNodes() > L.MaxNodes)
    return std::nullopt;
  auto Constraints = conjuncts(C, Full, R), Terms = conjuncts(C, P.Goal, R);
  if (!Constraints || !Terms)
    return std::nullopt;
  P.Constraints.clear();
  for (auto [F, Positive] : *Constraints)
    P.Constraints.push_back(Positive ? F : C.mkNot(F));
  for (auto [F, Positive] : *Terms)
    P.Terms.push_back(Positive ? F : C.mkNot(F));
  if (C.numNodes() > L.MaxNodes)
    return std::nullopt;
  P.PreparationWork = B.Used + R.Used;
  P.CollectionWork = B.Used;
  P.RebuildWork = R.Used;
  return P;
}

inline std::optional<Prepared> prepare(SymContext &C, SymRef Domain,
                                       SymRef Goal, const Limits &L = {}) {
  const size_t OriginalSize = C.numNodes();
  if (!Domain || !Goal || Domain.index() >= OriginalSize ||
      Goal.index() >= OriginalSize || C.width(Domain) != 1 ||
      C.width(Goal) != 1 || OriginalSize > L.MaxNodes)
    return std::nullopt;
  Budget Work(std::min<uint64_t>(L.MaxWork, 4194304));
  std::vector<SymRef> Pending{Domain, Goal}, Refs;
  llvm::DenseSet<uint32_t> Seen;
  while (!Pending.empty()) {
    if (!Work.charge())
      return std::nullopt;
    auto At = Pending.back();
    Pending.pop_back();
    if (!At || At.index() >= OriginalSize)
      return std::nullopt;
    if (!Seen.insert(At.index()).second)
      continue;
    if (Seen.size() > std::min(L.MaxReachableNodes, 262144U) || !C.width(At) ||
        C.width(At) > std::min(L.MaxWidth, 256U))
      return std::nullopt;
    const auto Args = C.operands(At);
    if (!Work.charge(Args.size()) || !detail::validProofNode(C, At))
      return std::nullopt;
    for (auto A : Args) {
      if (!A || A.index() >= At.index())
        return std::nullopt;
      Pending.push_back(A);
    }
    Refs.push_back(At);
  }
  std::sort(Refs.begin(), Refs.end());
  llvm::DenseMap<uint32_t, SymRef> Map;
  std::vector<SymRef> Images;
  for (auto At : Refs) {
    if (!Work.charge(1 + C.numOperands(At)))
      return std::nullopt;
    std::vector<SymRef> Args;
    for (auto A : C.operands(At))
      Args.push_back(Map.at(A.index()));
    // Keep the authoritative rebuild dispatch; only Boolean equality has
    // an additional, independently validated proof preparation fold.
    auto Image = C.rebuild(At, Args);
    if (C.op(Image) == SymOp::Eq) {
      const auto A = C.operand(Image, 0), B = C.operand(Image, 1);
      Image = booleanEquality(C, A, B, &Work);
    }
    if (!Work.Active || !Image || C.numNodes() > L.MaxNodes)
      return std::nullopt;
    Map[At.index()] = Image;
    Images.push_back(Image);
  }
  auto P = prepareCanonical(C, Map.at(Domain.index()), Map.at(Goal.index()), L);
  if (!P)
    return std::nullopt;
  P->SourceContext = &C;
  P->SourceDomain = Domain;
  P->SourceGoal = Goal;
  P->NormalizationWork = Work.Used;
  P->NormalizationOriginal = std::move(Refs);
  P->NormalizationImages = std::move(Images);
  return P;
}

inline std::unique_ptr<solver::BitVectorSolver>
encode(SymContext &C, const Prepared &P, const solver::SolverOptions &Options,
       const Limits &L = {}) {
  Budget Work(std::min<uint64_t>(L.MaxWork, 4194304));
  std::vector<std::pair<unsigned, SymRef>> Ordered;
  for (auto F : P.Constraints) {
    std::vector<SymRef> Pending{F};
    llvm::DenseSet<uint32_t> Seen;
    unsigned Cost = 0;
    while (!Pending.empty() && Cost < 4096) {
      if (!Work.charge())
        return nullptr;
      ++Cost;
      auto At = Pending.back();
      Pending.pop_back();
      if (!Seen.insert(At.index()).second)
        continue;
      for (auto A : C.operands(At)) {
        if (++Cost >= 4096)
          break;
        if (!Work.charge())
          return nullptr;
        Pending.push_back(A);
      }
    }
    Ordered.push_back({Cost, F});
  }
  std::sort(Ordered.begin(), Ordered.end(), [](auto A, auto B) {
    return A.first != B.first ? A.first < B.first : A.second < B.second;
  });
  auto O = Options;
  O.BuildModel = false;
  auto Base = std::make_unique<solver::BitVectorSolver>(C, O);
  for (auto [Cost, F] : Ordered)
    if (!Base->assertTrue(F))
      return nullptr;
  for (auto Input : P.Inputs) {
    solver::BitLits Bits;
    if (!Base->blaster().blast(Input, Bits))
      return nullptr;
  }
  return Base;
}

inline Result proveWithEncoding(SymContext &C, const Prepared &P,
                                solver::BitVectorSolver &Base,
                                llvm::function_ref<bool()> ChargeQuery,
                                const Limits &L = {}) {
  Result Out;
  Out.TotalTerms = P.Terms.size();
  Out.DomainGates = Base.blaster().encoder().numGates();
  std::vector<std::pair<unsigned, unsigned>> Pending{
      {0, unsigned(P.Terms.size())}};
  while (!Pending.empty()) {
    if (Out.Queries >= std::min(L.MaxQueries, 16384U) ||
        C.numNodes() > L.MaxNodes)
      return Out;
    const auto [Begin, Count] = Pending.back();
    Pending.pop_back();
    if (!Count)
      return Out;
    const auto Group =
        C.mkAnd(llvm::ArrayRef<SymRef>(P.Terms).slice(Begin, Count));
    const auto Q = C.mkNot(Group);
    if (C.numNodes() > L.MaxNodes || !ChargeQuery())
      return Out;
    ++Out.Queries;
    if (C.numNodes() > L.MaxNodes)
      return Out;
    auto Clone = Base.cloneEncoding();
    if (!Clone)
      return Out;
    Clone->assertTrue(Q);
    const auto Answer = Clone->check();
    Out.Batches.push_back(
        {Begin, Count, Answer, Clone->blaster().encoder().numGates()});
    if (Answer == SatResult::Unsat) {
      Out.ProvedTerms += Count;
      continue;
    }
    if (Answer != SatResult::Unknown || Count == 1)
      return Out;
    const unsigned Left = Count / 2;
    Pending.push_back({Begin + Left, Count - Left});
    Pending.push_back({Begin, Left});
  }
  Out.Proved = Out.ProvedTerms == Out.TotalTerms;
  return Out;
}
inline Result prove(SymContext &C, const Prepared &P,
                    const solver::SolverOptions &Options,
                    llvm::function_ref<bool()> ChargeQuery,
                    const Limits &L = {}) {
  Result Out;
  Out.TotalTerms = P.Terms.size();
  auto Base = encode(C, P, Options, L);
  if (!Base)
    return Out;
  return proveWithEncoding(C, P, *Base, ChargeQuery, L);
}
// Stores conditional identities and pristine K encoding, never a query answer.
// Context, domain, solver options and preparation limits are fixed for its
// life.
class CachedDomain {
  SymContext &C;
  SymRef Domain;
  Limits Bound;
  llvm::DenseMap<uint32_t, SymRef> Images;
  std::unique_ptr<solver::BitVectorSolver> Encoding;

  CachedDomain(SymContext &Context, SymRef D, const Limits &L)
      : C(Context), Domain(D), Bound(L) {}

public:
  bool matchesDomain(SymRef D) const { return D == Domain; }

  static std::unique_ptr<CachedDomain>
  create(SymContext &C, SymRef Domain, const Prepared &P,
         const solver::SolverOptions &Options, const Limits &L = {}) {
    if (P.SourceContext != &C || P.SourceDomain != Domain ||
        C.numNodes() > L.MaxNodes || P.Original.size() != P.Images.size() ||
        P.NormalizationOriginal.size() != P.NormalizationImages.size() ||
        P.Original.size() > std::min(L.MaxReachableNodes, 262144U) ||
        P.NormalizationOriginal.size() > std::min(L.MaxReachableNodes, 262144U))
      return nullptr;
    auto Out = std::unique_ptr<CachedDomain>(new CachedDomain(C, Domain, L));
    Budget Work(std::min<uint64_t>(L.MaxWork, 4194304));
    const auto Valid = [&](SymRef Ref) {
      return Ref && Ref.index() < C.numNodes() && C.width(Ref) &&
             C.width(Ref) <= std::min(L.MaxWidth, 256U);
    };
    for (size_t I = 0; I != P.Original.size(); ++I) {
      if (!Work.charge() || !Valid(P.Original[I]) || !Valid(P.Images[I]))
        return nullptr;
      Out->Images[P.Original[I].index()] = P.Images[I];
    }
    for (size_t I = 0; I != P.NormalizationOriginal.size(); ++I) {
      const auto Before = P.NormalizationOriginal[I];
      const auto After = P.NormalizationImages[I];
      if (!Work.charge() || !Valid(Before) || !Valid(After))
        return nullptr;
      const auto Found = Out->Images.find(After.index());
      // Inserting Before can grow the map and invalidate Found. Copy the
      // mapped value before operator[] rather than retaining its reference.
      const SymRef Image = Found == Out->Images.end() ? After : Found->second;
      Out->Images[Before.index()] = Image;
    }
    Out->Encoding = encode(C, P, Options, L);
    return Out->Encoding ? std::move(Out) : nullptr;
  }

  std::optional<Prepared> rewrite(SymRef D, SymRef Goal) {
    if (D != Domain || !Goal || Goal.index() >= C.numNodes() ||
        C.width(Goal) != 1 || C.numNodes() > Bound.MaxNodes)
      return std::nullopt;
    const auto OriginalSize = C.numNodes();
    Budget Work(std::min<uint64_t>(Bound.MaxWork, 4194304));
    std::vector<SymRef> Pending{Goal}, Refs;
    llvm::DenseSet<uint32_t> Seen;
    while (!Pending.empty()) {
      if (!Work.charge())
        return std::nullopt;
      auto At = Pending.back();
      Pending.pop_back();
      if (!At || At.index() >= OriginalSize)
        return std::nullopt;
      if (!Seen.insert(At.index()).second)
        continue;
      if (Seen.size() > std::min(Bound.MaxReachableNodes, 262144U) ||
          !C.width(At) || C.width(At) > std::min(Bound.MaxWidth, 256U))
        return std::nullopt;
      Refs.push_back(At);
      if (Images.contains(At.index()))
        continue;
      const auto Args = C.operands(At);
      if (!Work.charge(Args.size()) || !detail::validProofNode(C, At))
        return std::nullopt;
      for (auto A : Args) {
        if (!A || A.index() >= At.index())
          return std::nullopt;
        Pending.push_back(A);
      }
    }
    std::sort(Refs.begin(), Refs.end());
    llvm::DenseMap<uint32_t, SymRef> Local;
    for (auto At : Refs) {
      if (!Work.charge(1 + C.numOperands(At)))
        return std::nullopt;
      if (const auto Found = Images.find(At.index()); Found != Images.end()) {
        Local[At.index()] = Found->second;
        continue;
      }
      std::vector<SymRef> Args;
      for (auto A : C.operands(At))
        Args.push_back(Local.at(A.index()));
      auto Image = C.rebuild(At, Args);
      if (C.op(Image) == SymOp::Eq)
        Image =
            booleanEquality(C, C.operand(Image, 0), C.operand(Image, 1), &Work);
      if (!Work.Active || !Image || C.numNodes() > Bound.MaxNodes)
        return std::nullopt;
      Local[At.index()] = Image;
    }
    Prepared P;
    P.Goal = Local.at(Goal.index());
    auto Terms = conjuncts(C, P.Goal, Work);
    if (!Terms)
      return std::nullopt;
    for (auto [Term, Positive] : *Terms)
      P.Terms.push_back(Positive ? Term : C.mkNot(Term));
    if (C.numNodes() > Bound.MaxNodes)
      return std::nullopt;
    P.NormalizationWork = Work.Used;
    return P;
  }

  Result prove(SymRef D, SymRef Goal, llvm::function_ref<bool()> ChargeQuery) {
    auto P = rewrite(D, Goal);
    if (!P)
      return {};
    auto Out = proveWithEncoding(C, *P, *Encoding, ChargeQuery, Bound);
    Out.NormalizationWork = P->NormalizationWork;
    return Out;
  }
};
inline auto policy(const solver::SolverOptions &O, const Limits &Bound) {
  return std::tuple{
      Bound.MaxNodes,       Bound.MaxWork,         Bound.MaxReachableNodes,
      Bound.MaxWidth,       Bound.MaxQueries,      O.Blast.MaxWidth,
      O.Blast.MaxGates,     O.BuildModel,          O.Sat.VarDecay,
      O.Sat.ClauseDecay,    O.Sat.RestartInterval, O.Sat.LearnedFraction,
      O.Sat.LearnedGrowth,  O.Sat.MaxConflicts,    O.Sat.MaxPropagations,
      O.Sat.MaxWatchVisits, O.Sat.MinimizeLearned, O.Sat.PhaseSaving,
      O.Sat.DefaultPhase};
}
using Policy = decltype(policy(solver::SolverOptions{}, Limits{}));
} // namespace

class Cache::Impl {
public:
  const symbolic::SymContext *Context;
  symbolic::SymRef Domain;
  Policy Configuration;
  std::unique_ptr<CachedDomain> Prepared;
  Impl(const symbolic::SymContext &Ctx, symbolic::SymRef D,
       Policy Configuration, std::unique_ptr<CachedDomain> P)
      : Context(&Ctx), Domain(D), Configuration(Configuration),
        Prepared(std::move(P)) {}
};
Cache::Cache() = default;
Cache::~Cache() = default;
Result Cache::proveCached(symbolic::SymContext &Ctx, symbolic::SymRef Domain,
                          symbolic::SymRef Goal,
                          const solver::SolverOptions &Options,
                          const Limits &Bound,
                          llvm::function_ref<bool()> ChargeQuery) {
  if (!State)
    return {};
  if (State->Context != &Ctx || State->Domain != Domain ||
      State->Configuration != policy(Options, Bound)) {
    State.reset();
    return {};
  }
  auto Out = State->Prepared->prove(Domain, Goal, ChargeQuery);
  Out.ReusedPreparation = true;
  if (Ctx.numNodes() > Bound.MaxNodes)
    Out.Proved = false;
  return Out;
}
Result Cache::proveFresh(symbolic::SymContext &Ctx, symbolic::SymRef Domain,
                         symbolic::SymRef Goal,
                         const solver::SolverOptions &Options,
                         const Limits &Bound, uint64_t RemainingQueries,
                         llvm::function_ref<bool()> ChargeQuery) {
  auto Attempt = Bound;
  Attempt.MaxQueries = std::min<uint64_t>(Bound.MaxQueries, RemainingQueries);
  const auto P = prepare(Ctx, Domain, Goal, Attempt);
  if (!P)
    return {};
  auto Out = prove(Ctx, *P, Options, ChargeQuery, Attempt);
  Out.NormalizationWork = P->NormalizationWork;
  Out.CollectionWork = P->CollectionWork;
  Out.ClosureWork = P->ClosureWork;
  Out.RebuildWork = P->RebuildWork;
  if (Out.Proved) {
    auto Retained = CachedDomain::create(Ctx, Domain, *P, Options, Bound);
    if (Ctx.numNodes() > Bound.MaxNodes)
      Out.Proved = false;
    else if (Retained)
      State = std::make_unique<Impl>(Ctx, Domain, policy(Options, Bound),
                                     std::move(Retained));
  }
  return Out;
}
} // namespace neverd::analysis::conditional_implication
