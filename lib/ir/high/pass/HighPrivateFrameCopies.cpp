//===- HighPrivateFrameCopies.cpp - Copies through private frame slots
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

// Binding first makes every call operand explicit. Once the frame cannot
// escape, available scalar leaves can survive calls and cross source branches.
// Stores replace a slot's current fact; writes invalidate facts depending on
// that source local. Joins and guard clones must agree before a statement is
// rewritten. The shared source graph owns reachability, and every incomplete
// proof or exhausted budget leaves the original function unchanged.

#include "HighDCEDetail.h"
#include "HighFrameAddress.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/high/HighSourceFlow.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {
namespace {
using Local = HighSourceLocalIdentity;

bool integerType(const TypeRef &T) {
  return T && T->Kind == NdTypeKind::Int &&
         (T->Size == 1 || T->Size == 2 || T->Size == 4 || T->Size == 8);
}

// A fact replays only a scalar leaf, never an operation, call or memory read.
// Equal-width integer views preserve its bits; the destination view is restored
// explicitly at each replacement, including C's narrow integer promotions.
ExprPtr copyLeaf(ExprPtr E) {
  for (unsigned Depth = 0; E && Depth != 16; ++Depth) {
    if (!integerType(E->Type) || E->Op != NdOp::NOP || E->IndirectTarget ||
        E->SourceCallHint || E->IntrinsicId != Intrinsic::None ||
        !E->IntrinsicOutputs.empty() ||
        E->MemoryOrdering != NdMemoryOrdering::None ||
        E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return nullptr;
    if (E->Kind == ExprKind::Var)
      return E->Operands.empty() && E->Type->Size == E->Var.Size &&
                     (E->Var.Kind == MedVar::Temp ||
                      E->Var.Kind == MedVar::Reg ||
                      E->Var.Kind == MedVar::Param)
                 ? E
                 : nullptr;
    if (E->Kind == ExprKind::Const)
      return E->Operands.empty() && E->AddressOwnerVA == InvalidVA &&
                     (E->ConstProvenance ==
                          ConstantAddressProvenance::Unknown ||
                      E->ConstProvenance == ConstantAddressProvenance::Scalar)
                 ? E
                 : nullptr;
    if (E->Kind != ExprKind::Cast || E->Operands.size() != 1 ||
        !E->Operands[0] || !integerType(E->Operands[0]->Type) ||
        E->Operands[0]->Type->Size != E->Type->Size ||
        !integerType(E->CastTo) || E->CastTo->Size != E->Type->Size ||
        E->CastTo->IsSigned != E->Type->IsSigned)
      return nullptr;
    E = E->Operands[0];
  }
  return nullptr;
}

bool sameCopy(const ExprPtr &L, const ExprPtr &R) {
  if (L->Type->Size != R->Type->Size || L->Kind != R->Kind)
    return false;
  if (L->Kind == ExprKind::Var)
    return highSourceLocalIdentity(L->Var) == highSourceLocalIdentity(R->Var);
  return L->structuralEq(*R);
}

bool dependsOn(const ExprPtr &E, const Local &K) {
  return E->Kind == ExprKind::Var && highSourceLocalIdentity(E->Var) == K;
}

class PrivateFrameCopies {
  const HighFunc &Function;
  Arch Architecture;
  size_t Budget = 1000000;
  bool Safe = true;
  std::vector<const HighStmt *> Statements;
  std::map<Local, std::vector<ExprPtr>> Definitions;
  std::set<Local> Parameters;
  std::set<Local> NonPointerDefinitions;
  std::map<Local, ExprPtr> Aliases;
  std::map<Local, bool> Derived;
  std::set<Local> Deriving;
  struct Slot {
    uint16_t Bytes;
    bool Eligible;
  };
  std::map<int64_t, Slot> Slots;
  struct State {
    std::map<int64_t, ExprPtr> Memory;
    std::map<Local, ExprPtr> Copies;
    size_t size() const { return Memory.size() + Copies.size(); }
  };
  std::map<const HighStmt *, State> Before;

  bool spend(size_t Count = 1) {
    if (!Safe || Count > Budget) {
      Safe = false;
      return false;
    }
    Budget -= Count;
    return true;
  }

  std::optional<int64_t> offset(const ExprPtr &E) {
    const auto Alias = [&](const MedVar &V) -> ExprPtr {
      auto I = Aliases.find(highSourceLocalIdentity(V));
      return I == Aliases.end() ? nullptr : I->second;
    };
    return high_detail::frameAddressOffset(E, Function, Architecture, Budget, 0,
                                           Alias);
  }

  bool frameDerived(const ExprPtr &E, unsigned Depth = 0) {
    if (!E)
      return false;
    if (!spend() || Depth > 64) {
      Safe = false;
      return true;
    }
    // A load or call cannot produce this private frame's address unless that
    // address has already escaped. All such stores and call operands are
    // rejected by inspect(), independently of whether their results are used.
    if (E->Kind == ExprKind::Load || E->Kind == ExprKind::Call)
      return false;
    if (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) {
      if (isSyntheticEntryStackPointer(E->Var, Function, Architecture))
        return true;
      const auto K = highSourceLocalIdentity(E->Var);
      if (Aliases.count(K))
        return true;
      if (auto I = Derived.find(K); I != Derived.end())
        return I->second;
      if (!Deriving.insert(K).second) {
        Safe = false;
        return true;
      }
      bool Result = false;
      if (auto I = Definitions.find(K); I != Definitions.end())
        for (const auto &D : I->second)
          Result |= frameDerived(D, Depth + 1);
      Deriving.erase(K);
      Derived[K] = Result;
      return Result;
    }
    for (const auto &Child : E->Operands)
      if (frameDerived(Child, Depth + 1))
        return true;
    return false;
  }

  void access(const ExprPtr &Address, const TypeRef &Type) {
    if (!Type || !Type->Size) {
      Safe = false;
      return;
    }
    const auto At = offset(Address);
    if (!At) {
      if (frameDerived(Address))
        Safe = false;
      return;
    }
    if (*At < -Function.FrameSize || *At >= 0 ||
        uint64_t(Type->Size) > uint64_t(-*At)) {
      Safe = false;
      return;
    }
    auto [I, Fresh] = Slots.emplace(*At, Slot{Type->Size, integerType(Type)});
    if (!Fresh && (I->second.Bytes != Type->Size || !integerType(Type)))
      I->second.Eligible = false;
    for (auto &[Other, S] : Slots) {
      if (!spend())
        return;
      if (Other != *At && Other + S.Bytes > *At && *At + Type->Size > Other)
        S.Eligible = I->second.Eligible = false;
    }
  }

  bool collect() {
    std::vector<std::pair<const HighStmt *, unsigned>> Work;
    std::set<const HighExpr *> Seen;
    for (const auto &S : Function.Body)
      Work.emplace_back(&S, 0);
    while (!Work.empty()) {
      const auto [S, Depth] = Work.back();
      Work.pop_back();
      if (!spend() || Depth > 200)
        return false;
      Statements.push_back(S);
      forEachExpr(*S, [&](const ExprPtr &Root) {
        std::vector<ExprPtr> Pending{Root};
        while (!Pending.empty()) {
          const auto E = Pending.back();
          Pending.pop_back();
          if (!spend())
            return;
          if (!Seen.insert(E.get()).second)
            continue;
          if ((E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) &&
              E->Var.Kind == MedVar::Param)
            Parameters.insert(highSourceLocalIdentity(E->Var));
          E->forEachChildExpr([&](const ExprPtr &C) { Pending.push_back(C); });
        }
      });
      if (!S->EHClauses.empty() || !S->EHClauseBodies.empty() ||
          S->MemoryOrdering != NdMemoryOrdering::None ||
          S->MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return false;
      if ((S->Dst && S->Kind != StmtKind::Assign) ||
          (S->Val && S->Kind != StmtKind::Assign &&
           S->Kind != StmtKind::ExprStmt) ||
          (S->Cond && S->Kind != StmtKind::If && S->Kind != StmtKind::IfElse) ||
          (S->RetVal && S->Kind != StmtKind::Return) ||
          (S->StoreAddr && S->Kind != StmtKind::Store) ||
          (S->StoreVal && S->Kind != StmtKind::Store) ||
          (S->CallExpr && S->Kind != StmtKind::Call) ||
          (S->SwitchExpr && S->Kind != StmtKind::Switch))
        return false;
      switch (S->Kind) {
      case StmtKind::If:
      case StmtKind::IfElse:
        if (!S->Cases.empty() || !S->DefaultBody.empty())
          return false;
        break;
      case StmtKind::Switch:
        if (!S->Body.empty() || !S->ElseBody.empty())
          return false;
        break;
      case StmtKind::Block:
        if (!S->ElseBody.empty() || !S->Cases.empty() ||
            !S->DefaultBody.empty())
          return false;
        break;
      case StmtKind::While:
      case StmtKind::DoWhile:
      case StmtKind::For:
      case StmtKind::SEHTry:
      case StmtKind::CxxTry:
      case StmtKind::ItaniumTry:
        return false;
      default:
        if (!S->Body.empty() || !S->ElseBody.empty() || !S->Cases.empty() ||
            !S->DefaultBody.empty())
          return false;
      }
      if (S->Kind == StmtKind::Assign) {
        if (!S->Dst || !S->Val || S->Dst->Kind != ExprKind::Var ||
            isSyntheticEntryStackPointer(S->Dst->Var, Function, Architecture))
          return false;
        const auto K = highSourceLocalIdentity(S->Dst->Var);
        Definitions[K].push_back(S->Val);
        if (!integerType(S->Dst->Type) ||
            S->Dst->Type->Size != getTargetRegInfo(Architecture).PointerSize ||
            S->Dst->Var.Size != S->Dst->Type->Size)
          NonPointerDefinitions.insert(K);
      }
      const auto Append = [&](const std::vector<HighStmt> &Body) {
        for (const auto &Child : Body)
          Work.emplace_back(&Child, Depth + 1);
      };
      Append(S->Body);
      Append(S->ElseBody);
      Append(S->DefaultBody);
      for (const auto &Case : S->Cases)
        Append(Case.Body);
    }
    // A source local can have several definitions, but every definition must
    // produce the identical frame address. Definite assignment below also
    // proves that each frame-derived use has a reaching definition.
    for (unsigned Pass = 0; Pass != 64 && Safe; ++Pass) {
      bool Changed = false;
      for (const auto &[K, Defs] : Definitions) {
        if (!spend())
          return false;
        // Parameters have an implicit entry definition. A later assignment
        // of a frame address cannot turn their earlier values into aliases.
        if (Aliases.count(K) || Parameters.count(K) ||
            NonPointerDefinitions.count(K))
          continue;
        std::optional<int64_t> Common;
        bool Agrees = true;
        for (const auto &E : Defs) {
          const auto At = offset(E);
          if (!At || (Common && *Common != *At)) {
            Agrees = false;
            break;
          }
          Common = At;
        }
        if (Agrees && Common) {
          Aliases.emplace(K, Defs.front());
          Changed = true;
        }
      }
      if (!Changed)
        break;
    }
    return Safe && Budget;
  }

  bool inspect() {
    const auto Report = analyzeHighSourceFlow(Function, false);
    if (!Report.Complete)
      return false;
    for (const auto &D : Report.Items) {
      if (D.Issue != HighSourceFlowIssue::DefiniteAssignment || !D.Expression)
        return false;
      ExprPtr E(const_cast<HighExpr *>(D.Expression), [](HighExpr *) {});
      if (frameDerived(E))
        return false;
    }
    std::set<const HighExpr *> Seen, Active;
    const auto Visit = [&](auto &&Self, const ExprPtr &E,
                           unsigned Depth) -> void {
      if (!E || !spend() || Depth > 200) {
        Safe = false;
        return;
      }
      if (Seen.count(E.get()))
        return;
      switch (E->Kind) {
      case ExprKind::Var:
      case ExprKind::Const:
      case ExprKind::Undef:
      case ExprKind::Phi:
        if (!E->Operands.empty()) {
          Safe = false;
          return;
        }
        break;
      case ExprKind::Load:
      case ExprKind::Cast:
      case ExprKind::BitCast:
      case ExprKind::UnaryOp:
        if (E->Operands.size() != 1) {
          Safe = false;
          return;
        }
        break;
      case ExprKind::BinOp:
        if (E->Operands.size() != (E->Op == NdOp::FLOAT_FMA ? 3u : 2u)) {
          Safe = false;
          return;
        }
        break;
      default:
        break;
      }
      if (!Active.insert(E.get()).second || E->IntrinsicId != Intrinsic::None ||
          !E->IntrinsicOutputs.empty() ||
          E->MemoryOrdering != NdMemoryOrdering::None ||
          E->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          E->Op == NdOp::ATOMIC_ADD || E->Op == NdOp::ATOMIC_XCHG ||
          E->Op == NdOp::ATOMIC_CMPXCHG || E->Kind == ExprKind::Addr ||
          E->Kind == ExprKind::Store ||
          (E->Kind == ExprKind::Var && E->Var.Kind == MedVar::Stack) ||
          (E->IndirectTarget && E->Kind != ExprKind::Call)) {
        Safe = false;
        return;
      }
      if (E->Kind == ExprKind::Load) {
        if (E->Operands.size() != 1) {
          Safe = false;
          return;
        }
        access(E->Operands[0], E->Type);
      }
      if ((E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) &&
          frameDerived(E) &&
          (!integerType(E->Type) ||
           E->Type->Size != getTargetRegInfo(Architecture).PointerSize ||
           E->Var.Size != E->Type->Size)) {
        Safe = false;
        return;
      }
      if (E->Kind == ExprKind::Call) {
        std::string Reason;
        if (!E->SourceCallHint || E->IndirectParamIdx >= 0 ||
            !validateSourceABI(E->SourceCallHint->Signature, Reason) ||
            E->Operands.size() !=
                E->SourceCallHint->Signature.Parameters.size() ||
            frameDerived(E->IndirectTarget)) {
          Safe = false;
          return;
        }
        for (const auto &Child : E->Operands)
          if (frameDerived(Child)) {
            Safe = false;
            return;
          }
      }
      for (const auto &Child : E->Operands)
        Self(Self, Child, Depth + 1);
      if (E->IndirectTarget)
        Self(Self, E->IndirectTarget, Depth + 1);
      Active.erase(E.get());
      Seen.insert(E.get());
    };
    for (const auto *S : Statements) {
      if (S->Kind == StmtKind::Store) {
        if (!S->StoreAddr || !S->StoreVal || frameDerived(S->StoreVal))
          return false;
        access(S->StoreAddr, S->StoreVal->Type);
      }
      if (S->Kind == StmtKind::Return && frameDerived(S->RetVal))
        return false;
      forEachExpr(*S, [&](const ExprPtr &E) { Visit(Visit, E, 0); });
      if (!Safe)
        return false;
    }
    return Safe && Budget && !Slots.empty();
  }

  void intersect(State &Dest, const State &From) {
    const auto Intersect = [&](auto &D, const auto &S) {
      for (auto I = D.begin(); I != D.end();) {
        if (!spend())
          return;
        const auto J = S.find(I->first);
        if (J == S.end() || !sameCopy(I->second, J->second))
          I = D.erase(I);
        else
          ++I;
      }
    };
    Intersect(Dest.Memory, From.Memory);
    Intersect(Dest.Copies, From.Copies);
  }

  ExprPtr replace(const ExprPtr &Root, const State &S) {
    std::map<const HighExpr *, ExprPtr> Cache;
    const auto Visit = [&](auto &&Self, const ExprPtr &E,
                           unsigned Depth) -> ExprPtr {
      if (!E)
        return E;
      if (!spend() || Depth > 200) {
        Safe = false;
        return E;
      }
      if (auto I = Cache.find(E.get()); I != Cache.end())
        return I->second;
      ExprPtr Value;
      if (E->Kind == ExprKind::Var && copyLeaf(E)) {
        const auto I = S.Copies.find(highSourceLocalIdentity(E->Var));
        if (I != S.Copies.end())
          Value = I->second;
      }
      if (E->Kind == ExprKind::Load && integerType(E->Type)) {
        const auto At = offset(E->Operands[0]);
        const auto I = At ? S.Memory.find(*At) : S.Memory.end();
        if (I != S.Memory.end())
          Value = I->second;
      }
      if (Value && Value->Type->Size == E->Type->Size) {
        auto View = std::make_shared<HighExpr>();
        View->Kind = ExprKind::Cast;
        View->Type = View->CastTo = E->Type;
        View->Operands = {Value};
        Cache[E.get()] = View;
        return View;
      }
      auto Copy = std::make_shared<HighExpr>(*E);
      bool Changed = false;
      for (auto &Child : Copy->Operands) {
        const auto R = Self(Self, Child, Depth + 1);
        Changed |= R != Child;
        Child = R;
      }
      Copy->IndirectTarget = Self(Self, E->IndirectTarget, Depth + 1);
      Changed |= Copy->IndirectTarget != E->IndirectTarget;
      Cache[E.get()] = Changed ? Copy : E;
      return Cache.at(E.get());
    };
    return Visit(Visit, Root, 0);
  }

  bool solve() {
    const auto Graph = buildHighSourceFlowGraph(Function);
    if (!Graph.Diagnostics.Complete || Graph.Nodes.empty())
      return false;
    std::vector<bool> Reachable(Graph.Nodes.size());
    std::vector<size_t> Degree(Graph.Nodes.size()), Work{Graph.Entry};
    while (!Work.empty()) {
      const auto I = Work.back();
      Work.pop_back();
      if (!spend())
        return false;
      if (Reachable[I])
        continue;
      Reachable[I] = true;
      for (const auto Next : Graph.Nodes[I].Successors) {
        ++Degree[Next];
        Work.push_back(Next);
      }
    }
    if (Degree[Graph.Entry])
      return false;
    Work = {Graph.Entry};
    std::vector<std::optional<State>> Incoming(Graph.Nodes.size());
    Incoming[Graph.Entry] = State{};
    size_t Visited = 0;
    while (!Work.empty()) {
      const auto I = Work.back();
      Work.pop_back();
      ++Visited;
      if (!Incoming[I] || !spend(1 + Incoming[I]->size()))
        return false;
      State S = std::move(*Incoming[I]);
      Incoming[I].reset();
      if (const auto *T = Graph.Nodes[I].Statement) {
        if (!spend(S.size()))
          return false;
        auto [At, Fresh] = Before.emplace(T, S);
        if (!Fresh)
          intersect(At->second, S);
        if (T->Kind == StmtKind::Assign) {
          const auto K = highSourceLocalIdentity(T->Dst->Var);
          auto V = copyLeaf(replace(T->Val, S));
          if (!spend(S.size()))
            return false;
          for (auto J = S.Copies.begin(); J != S.Copies.end();)
            if (J->first == K || dependsOn(J->second, K))
              J = S.Copies.erase(J);
            else
              ++J;
          for (auto J = S.Memory.begin(); J != S.Memory.end();)
            if (dependsOn(J->second, K))
              J = S.Memory.erase(J);
            else
              ++J;
          if (V && !frameDerived(V) && !dependsOn(V, K) && copyLeaf(T->Dst) &&
              T->Dst->Type->Size == V->Type->Size)
            S.Copies[K] = V;
        }
        if (T->Kind == StmtKind::Store) {
          if (const auto At = offset(T->StoreAddr)) {
            S.Memory.erase(*At);
            const auto Slot = Slots.find(*At);
            auto V = copyLeaf(replace(T->StoreVal, S));
            if (Slot != Slots.end() && Slot->second.Eligible && V &&
                !frameDerived(V))
              S.Memory[*At] = V;
          }
        }
      }
      for (const auto Next : Graph.Nodes[I].Successors) {
        if (!spend(1 + S.size()))
          return false;
        if (Incoming[Next])
          intersect(*Incoming[Next], S);
        else
          Incoming[Next] = S;
        if (!--Degree[Next])
          Work.push_back(Next);
      }
    }
    // Loops need a fixed-point proof, not a single speculative traversal.
    return Safe && Budget &&
           Visited ==
               size_t(std::count(Reachable.begin(), Reachable.end(), true));
  }

public:
  PrivateFrameCopies(const HighFunc &F, Arch A)
      : Function(F), Architecture(A) {}

  bool run(HighFunc &Output) {
    if (Function.FrameSize <= 0 || Function.StructuredExceptionRegions ||
        Function.UnstructuredExceptionRegions ||
        (Architecture != Arch::AArch64 && Architecture != Arch::X64) ||
        !collect() || !inspect() || !solve())
      return false;
    HighFunc Result = Function;
    std::vector<HighStmt *> Targets;
    std::vector<HighStmt *> Work;
    for (auto &S : Result.Body)
      Work.push_back(&S);
    while (!Work.empty()) {
      auto *S = Work.back();
      Work.pop_back();
      Targets.push_back(S);
      const auto Append = [&](std::vector<HighStmt> &Body) {
        for (auto &Child : Body)
          Work.push_back(&Child);
      };
      Append(S->Body);
      Append(S->ElseBody);
      Append(S->DefaultBody);
      for (auto &Case : S->Cases)
        Append(Case.Body);
    }
    bool Changed = false;
    for (size_t I = 0; I != Targets.size(); ++I) {
      auto &S = *Targets[I];
      const auto At = Before.find(Statements[I]);
      if (At == Before.end())
        continue;
      forEachRhsExpr(S, [&](ExprPtr &E) {
        const auto R = replace(E, At->second);
        Changed |= R != E;
        E = R;
      });
      if (S.Kind == StmtKind::Assign && copyLeaf(S.Dst) && copyLeaf(S.Val) &&
          S.Dst->Type->Size == S.Val->Type->Size && !frameDerived(S.Val))
        S.IsPhiCopy = true;
    }
    if (!Changed || !Safe || !Budget)
      return false;
    std::set<int64_t> Read;
    std::set<const HighExpr *> Seen;
    for (const auto *S : Targets)
      forEachRhsExpr(*S, [&](const ExprPtr &Root) {
        std::vector<ExprPtr> Pending{Root};
        while (!Pending.empty()) {
          const auto E = Pending.back();
          Pending.pop_back();
          if (!spend())
            return;
          if (!Seen.insert(E.get()).second)
            continue;
          if (E->Kind == ExprKind::Load)
            if (const auto At = offset(E->Operands[0]))
              Read.insert(*At);
          E->forEachChildExpr([&](const ExprPtr &C) { Pending.push_back(C); });
        }
      });
    for (auto *S : Targets) {
      if (S->Kind != StmtKind::Store || !copyLeaf(S->StoreVal))
        continue;
      const auto At = offset(S->StoreAddr);
      if (!At || Read.count(*At))
        continue;
      const auto I = Slots.find(*At);
      if (I == Slots.end() || !I->second.Eligible)
        continue;
      const va_t Address = S->Addr;
      *S = HighStmt{};
      S->Addr = Address;
    }
    if (!Safe || !Budget)
      return false;
    Output = std::move(Result);
    return true;
  }
};
} // namespace

bool forwardBoundPrivateFrameCopies(HighFunc &Function, Arch Architecture) {
  return PrivateFrameCopies(Function, Architecture).run(Function);
}
} // namespace neverd
