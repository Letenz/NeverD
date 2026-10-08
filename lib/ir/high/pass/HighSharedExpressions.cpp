//===- HighSharedExpressions.cpp - Name values a statement repeats --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A HighIR expression is a DAG: a value inlined where several operations
/// read it is one node with several parents, and the C writer prints it at
/// every parent.  Lane-wise vector code shares its values level after level,
/// so one statement could print megabytes.  A large value a statement reads
/// more than once is assigned to a fresh local right before the statement,
/// which then reads the local.  Integer arithmetic on locals and constants
/// cannot fault or have an effect, so it may run earlier and outside a
/// branch of the statement.  A memory read moves only out of a statement
/// with no effect of its own before its end, where the read always runs.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/MedToHigh.h"

#include "llvm/ADT/STLExtras.h"

#include <cstdint>
#include <functional>
#include <set>
#include <unordered_map>

namespace neverd {
namespace {

/// A statement prints this many nodes at least before a repeated value is
/// worth a local, and at least this many times its distinct nodes unless it
/// exceeds kMaxPrintedNodes.
constexpr uint64_t kMinPrintedNodes = 256;
constexpr uint64_t kMinRepetition = 2;
/// A repeated value this small stays inline.
constexpr uint64_t kMinNamedNodes = 4;
/// A statement that still prints more nodes after its repeated values have
/// names prints its large movable parts as locals of about this size.
constexpr uint64_t kMaxPrintedNodes = 512;
constexpr uint64_t kPartNodes = 64;

/// Evaluating \p E cannot fault or have an effect.
bool effectFree(const HighExpr &E) {
  if (E.IntrinsicId != Intrinsic::None || !E.IntrinsicOutputs.empty() ||
      E.IndirectTarget || E.MemoryOrdering != NdMemoryOrdering::None ||
      E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  switch (E.Kind) {
  case ExprKind::Const:
    return true;
  case ExprKind::Var:
  case ExprKind::Phi:
    return E.Operands.empty() &&
           (E.Var.Kind == MedVar::Reg || E.Var.Kind == MedVar::Temp ||
            E.Var.Kind == MedVar::Param);
  case ExprKind::Cast:
  case ExprKind::BitCast:
    return E.Type && E.Type->Kind == NdTypeKind::Int;
  case ExprKind::UnaryOp:
    switch (E.Op) {
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
    case NdOp::INT_NEGATE:
    case NdOp::INT_NOT:
    case NdOp::INT_NEG2:
    case NdOp::BOOL_NOT:
    case NdOp::POPCOUNT:
      return true;
    default:
      return false;
    }
  case ExprKind::BinOp:
    switch (E.Op) {
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_MULT:
    case NdOp::INT_AND:
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
    case NdOp::INT_LEFT:
    case NdOp::INT_RIGHT:
    case NdOp::INT_ASHR:
    case NdOp::INT_EQUAL:
    case NdOp::INT_NOTEQUAL:
    case NdOp::INT_LESS:
    case NdOp::INT_LESSEQUAL:
    case NdOp::INT_SLESS:
    case NdOp::INT_SLESSEQUAL:
    case NdOp::BOOL_AND:
    case NdOp::BOOL_OR:
    case NdOp::BOOL_XOR:
    case NdOp::CONCAT:
    case NdOp::SELECT:
    case NdOp::INT_CARRY:
    case NdOp::INT_SOVF:
    case NdOp::INT_SBOR:
      return true;
    case NdOp::SUBBYTES:
      return E.Operands.size() == 2 && E.Operands[1] &&
             E.Operands[1]->Kind == ExprKind::Const;
    default:
      return false;
    }
  default:
    return false;
  }
}

/// A plain read of memory, which may fault, so it moves only from where it
/// always runs.
bool plainLoad(const HighExpr &E) {
  return E.Kind == ExprKind::Load && E.IntrinsicId == Intrinsic::None &&
         E.IntrinsicOutputs.empty() && !E.IndirectTarget &&
         E.Operands.size() == 1 && E.Type &&
         E.MemoryOrdering == NdMemoryOrdering::None &&
         E.MemoryAddressSpace == NdMemoryAddressSpace::Default;
}

/// Evaluating \p E may write memory or have another effect.
bool hasEffect(const HighExpr &E) {
  return E.Kind == ExprKind::Call || E.Kind == ExprKind::Store ||
         E.IntrinsicId != Intrinsic::None || !E.IntrinsicOutputs.empty() ||
         E.MemoryOrdering != NdMemoryOrdering::None ||
         E.MemoryAddressSpace != NdMemoryAddressSpace::Default;
}

/// The expression slots of \p S that the statement evaluates once, before
/// any effect of its own.  A loop condition runs again on every pass.
std::vector<ExprPtr *> evaluatedOnce(HighStmt &S) {
  switch (S.Kind) {
  case StmtKind::Assign:
    return {&S.Val};
  case StmtKind::Store:
    return {&S.StoreAddr, &S.StoreVal};
  case StmtKind::Return:
    return {&S.RetVal};
  case StmtKind::If:
  case StmtKind::IfElse:
    return {&S.Cond};
  case StmtKind::Switch:
    return {&S.SwitchExpr};
  case StmtKind::Call:
    return {&S.CallExpr};
  default:
    return {};
  }
}

class Namer {
public:
  explicit Namer(HighFunc &Func) : Func(Func) {
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
          S.GotoTarget != InvalidVA)
        Targets.insert(S.GotoTarget);
      for (const HighEHClause &Clause : S.EHClauses)
        Targets.insert(Clause.HandlerVA);
      forEachExpr(S, [&](const ExprPtr &E) { note(E); });
    });
  }

  bool run() { return visit(Func.Body); }

private:
  HighFunc &Func;
  std::set<va_t> Targets;
  std::set<const HighExpr *> Noted;
  int NextId = limits::kVarRenameIdBase;
  int NextTag = 0;
  Arch TheArch = Arch::Unknown;
  bool Changed = false;

  void note(const ExprPtr &E) {
    if (!E || !Noted.insert(E.get()).second)
      return;
    if (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) {
      NextId = std::max(NextId, E->Var.Id + 1);
      NextTag = std::max(NextTag, E->Var.RenameTag + 1);
      TheArch = E->Var.TheArch;
    }
    for (const ExprPtr &Operand : E->Operands)
      note(Operand);
    note(E->IndirectTarget);
  }

  bool visit(std::vector<HighStmt> &Body) {
    for (size_t I = 0; I < Body.size(); ++I) {
      for (std::vector<HighStmt> *Inner :
           {&Body[I].Body, &Body[I].ElseBody, &Body[I].DefaultBody})
        visit(*Inner);
      for (SwitchCase &Case : Body[I].Cases)
        visit(Case.Body);
      for (std::vector<HighStmt> &Clause : Body[I].EHClauseBodies)
        visit(Clause);
      std::vector<HighStmt> Named = name(Body[I]);
      if (Named.empty())
        continue;
      // A jump to the statement lands on the first new assignment instead,
      // which takes over its label.
      const va_t Address = Body[I].Addr;
      if (Address != 0 && Address != InvalidVA && Targets.count(Address))
        Named.front().Addr = Address;
      Body.insert(Body.begin() + static_cast<std::ptrdiff_t>(I),
                  std::make_move_iterator(Named.begin()),
                  std::make_move_iterator(Named.end()));
      I += Named.size();
      Changed = true;
    }
    return Changed;
  }

  /// The assignments that name the repeated values of \p S, in the order
  /// they run; \p S reads the names afterwards.
  std::vector<HighStmt> name(HighStmt &S) {
    std::vector<ExprPtr *> Roots = evaluatedOnce(S);
    // How many nodes the statement prints, and how many distinct ones.
    std::unordered_map<const HighExpr *, uint64_t> Printed;
    std::function<uint64_t(const HighExpr *)> Size =
        [&](const HighExpr *E) -> uint64_t {
      if (!E)
        return 0;
      if (auto It = Printed.find(E); It != Printed.end())
        return It->second;
      uint64_t N = 1;
      for (const ExprPtr &Operand : E->Operands)
        N = std::min<uint64_t>(N + Size(Operand.get()), UINT64_MAX / 4);
      N = std::min<uint64_t>(N + Size(E->IndirectTarget.get()),
                             UINT64_MAX / 4);
      Printed[E] = N;
      return N;
    };
    uint64_t Total = 0;
    for (ExprPtr *Root : Roots)
      Total += Size(Root->get());
    if (Total < kMinPrintedNodes ||
        (Total <= kMaxPrintedNodes && Total < kMinRepetition * Printed.size()))
      return {};

    // Work on a copy, so that a statement sharing these nodes keeps them.
    std::unordered_map<const HighExpr *, ExprPtr> Copies;
    std::function<ExprPtr(const ExprPtr &)> Copy =
        [&](const ExprPtr &E) -> ExprPtr {
      if (!E)
        return E;
      if (auto It = Copies.find(E.get()); It != Copies.end())
        return It->second;
      auto C = std::make_shared<HighExpr>(*E);
      Copies[E.get()] = C;
      for (ExprPtr &Operand : C->Operands)
        Operand = Copy(Operand);
      C->IndirectTarget = Copy(C->IndirectTarget);
      return C;
    };
    for (ExprPtr *Root : Roots)
      *Root = Copy(*Root);

    // The nodes evaluated only on some paths through the statement: the
    // right operand of a logical operator and the arms of a selection.
    bool Quiet = true;
    std::set<const HighExpr *> Conditional, Unconditional;
    std::function<void(const HighExpr *, bool)> Reach =
        [&](const HighExpr *E, bool Guarded) {
          if (!E || !(Guarded ? Conditional : Unconditional).insert(E).second)
            return;
          Quiet &= !hasEffect(*E);
          for (size_t I = 0; I < E->Operands.size(); ++I) {
            const bool Arm =
                (E->Kind == ExprKind::BinOp &&
                 (E->Op == NdOp::BOOL_AND || E->Op == NdOp::BOOL_OR) &&
                 I > 0) ||
                (E->Op == NdOp::SELECT && I > 0);
            Reach(E->Operands[I].get(), Guarded || Arm);
          }
          Reach(E->IndirectTarget.get(), Guarded);
        };
    for (ExprPtr *Root : Roots)
      Reach(Root->get(), false);
    // A destination in memory computes its address within the statement too.
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind != ExprKind::Var &&
        S.Dst->Kind != ExprKind::Phi)
      Reach(S.Dst.get(), false);

    // Each node's readers, and whether it may move before the statement.
    std::unordered_map<const HighExpr *, std::vector<ExprPtr *>> Readers;
    std::unordered_map<const HighExpr *, bool> Free;
    std::vector<ExprPtr> PostOrder;
    std::set<const HighExpr *> Seen;
    std::function<bool(ExprPtr &)> Walk = [&](ExprPtr &E) -> bool {
      if (!E)
        return true;
      Readers[E.get()].push_back(&E);
      if (!Seen.insert(E.get()).second)
        return Free[E.get()];
      bool AllFree = effectFree(*E) || (Quiet && plainLoad(*E) &&
                                        !Conditional.count(E.get()));
      for (ExprPtr &Operand : E->Operands)
        AllFree &= Walk(Operand);
      if (E->IndirectTarget) {
        Walk(E->IndirectTarget);
        AllFree = false;
      }
      Free[E.get()] = AllFree;
      PostOrder.push_back(E);
      return AllFree;
    };
    for (ExprPtr *Root : Roots)
      Walk(*Root);

    std::unordered_map<const HighExpr *, size_t> Position;
    for (size_t I = 0; I < PostOrder.size(); ++I)
      Position[PostOrder[I].get()] = I;
    // The new assignments with the position of the value each names.
    std::vector<std::pair<size_t, HighStmt>> Named;
    std::set<const ExprPtr *> RootSlots(Roots.begin(), Roots.end());
    // Assigns \p E to a fresh local, which its readers read instead.
    auto NameValue = [&](const ExprPtr &E) {
      if (NextTag >= INT16_MAX)
        return false;
      MedVar Local;
      Local.Kind = MedVar::Temp;
      Local.Id = NextId++;
      Local.RenameTag = static_cast<int16_t>(NextTag++);
      Local.Size = E->Type->Size;
      Local.TheArch = TheArch;
      HighStmt Assign;
      Assign.Kind = StmtKind::Assign;
      Assign.Dst = HighExpr::makeVar(Local, E->Type);
      Assign.Val = E;
      Assign.KeepsName = true;
      Named.emplace_back(Position[E.get()], std::move(Assign));
      for (ExprPtr *Use : Readers[E.get()])
        *Use = HighExpr::makeVar(Local, E->Type);
      // The sizes above shrink.
      Printed.clear();
      return true;
    };
    auto Nameable = [&](const ExprPtr &E) {
      return Free[E.get()] && E->Type && E->Kind != ExprKind::Var &&
             E->Kind != ExprKind::Phi && E->Kind != ExprKind::Const &&
             llvm::none_of(Readers[E.get()], [&](const ExprPtr *Use) {
               return RootSlots.count(Use) != 0;
             });
    };
    Printed.clear();
    // Children come before their parents, so a name is assigned before any
    // value reading it.
    for (const ExprPtr &E : PostOrder)
      if (Readers[E.get()].size() >= 2 && Nameable(E) &&
          Size(E.get()) >= kMinNamedNodes && !NameValue(E))
        break;
    uint64_t Remaining = 0;
    for (ExprPtr *Root : Roots)
      Remaining += Size(Root->get());
    if (Remaining > kMaxPrintedNodes)
      for (const ExprPtr &E : PostOrder)
        if (Readers[E.get()].size() == 1 && Nameable(E) &&
            Size(E.get()) >= kPartNodes && !NameValue(E))
          break;
    // A value a named one reads comes before it in the post order, so this
    // order assigns every name before any assignment reads it.
    llvm::stable_sort(Named, [](const auto &A, const auto &B) {
      return A.first < B.first;
    });
    std::vector<HighStmt> Ordered;
    for (auto &Entry : Named)
      Ordered.push_back(std::move(Entry.second));
    return Ordered;
  }
};

} // namespace

bool nameRepeatedValues(HighFunc &Func) { return Namer(Func).run(); }

} // namespace neverd
