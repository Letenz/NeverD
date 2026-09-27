//===- HighFrameStoreForwarding.cpp - Safe frame load forwarding ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Forward exact reads from immutable private stack slots before semantic
/// expression simplification. This recovers common inputs without guessing
/// about aliases, calls, or control flow.
///
//===----------------------------------------------------------------------===//

#include "HighDCEDetail.h"
#include "HighFrameAddress.h"

#include "neverd/ir/TargetRegInfo.h"

#include <algorithm>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neverd {

namespace {

constexpr size_t kMaxForwardValueNodes = 1024;
constexpr size_t kMaxForwardExpansionNodes = 4096;
constexpr size_t kFrameWalkBudget = 100000;

struct StoredFrameValue {
  int64_t Offset = 0;
  uint16_t Bytes = 0;
  TypeRef Type;
  ExprPtr Value;
  VarKeySet Dependencies;
  size_t Nodes = 0;
};

bool isForwardableInteger(const TypeRef &Type) {
  return Type && Type->Kind == NdTypeKind::Int &&
         (Type->Size == 1 || Type->Size == 2 || Type->Size == 4 ||
          Type->Size == 8);
}

bool sameIntegerType(const TypeRef &Left, const TypeRef &Right) {
  return isForwardableInteger(Left) && isForwardableInteger(Right) &&
         Left->Size == Right->Size && Left->IsSigned == Right->IsSigned;
}

bool rangesOverlap(int64_t Left, uint16_t LeftBytes, int64_t Right,
                   uint16_t RightBytes) {
  if (!LeftBytes || !RightBytes)
    return false;
  return Left <= Right ? uint64_t(Right) - uint64_t(Left) < LeftBytes
                       : uint64_t(Left) - uint64_t(Right) < RightBytes;
}

bool safeFrameValue(const ExprPtr &Root, VarKeySet &Dependencies,
                    size_t &NodeCount) {
  if (!Root)
    return false;
  std::vector<const HighExpr *> Work{Root.get()};
  std::unordered_set<const HighExpr *> Seen;
  while (!Work.empty()) {
    const HighExpr *Expr = Work.back();
    Work.pop_back();
    if (!Expr || !Seen.insert(Expr).second)
      continue;
    if (++NodeCount > kMaxForwardValueNodes ||
        Expr->IntrinsicId != Intrinsic::None ||
        !Expr->IntrinsicOutputs.empty() ||
        Expr->MemoryOrdering != NdMemoryOrdering::None ||
        Expr->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        (Expr->Type && Expr->Type->Kind != NdTypeKind::Int))
      return false;
    switch (Expr->Kind) {
    case ExprKind::Const:
      break;
    case ExprKind::Var:
      // A stack variable is a mutable memory home. Other lifted variables
      // retain their SSA identity; writes to one invalidate dependent slots.
      if (Expr->Var.Kind == MedVar::Stack)
        return false;
      Dependencies.insert(VK(Expr->Var));
      break;
    case ExprKind::BinOp:
    case ExprKind::UnaryOp:
    case ExprKind::Cast:
      break;
    default:
      return false;
    }
    Expr->forEachChildExpr(
        [&](const ExprPtr &Child) { Work.push_back(Child.get()); });
  }
  return true;
}

bool isStraightLinePrivateFrameFunction(const HighFunc &Func) {
  if (!Func.FrameSize || Func.StructuredExceptionRegions ||
      Func.UnstructuredExceptionRegions)
    return false;
  for (const HighStmt &Stmt : Func.Body) {
    if (!Stmt.Body.empty() || !Stmt.ElseBody.empty() || !Stmt.Cases.empty() ||
        !Stmt.DefaultBody.empty() || !Stmt.EHClauseBodies.empty() ||
        !Stmt.EHClauses.empty() || Stmt.IsPhiCopy ||
        Stmt.MemoryOrdering != NdMemoryOrdering::None ||
        Stmt.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;
    if (Stmt.Kind != StmtKind::Assign && Stmt.Kind != StmtKind::Store &&
        Stmt.Kind != StmtKind::ExprStmt && Stmt.Kind != StmtKind::Return &&
        Stmt.Kind != StmtKind::Nop)
      return false;
    bool Safe = true;
    forEachExpr(Stmt, [&](const ExprPtr &Root) {
      std::vector<const HighExpr *> Work{Root.get()};
      std::unordered_set<const HighExpr *> Seen;
      while (!Work.empty()) {
        const HighExpr *Expr = Work.back();
        Work.pop_back();
        if (!Expr || !Seen.insert(Expr).second)
          continue;
        if (Expr->Kind == ExprKind::Call || Expr->Kind == ExprKind::Addr ||
            Expr->Kind == ExprKind::Store ||
            Expr->IntrinsicId != Intrinsic::None ||
            !Expr->IntrinsicOutputs.empty() ||
            Expr->MemoryOrdering != NdMemoryOrdering::None ||
            Expr->MemoryAddressSpace != NdMemoryAddressSpace::Default) {
          Safe = false;
          return;
        }
        Expr->forEachChildExpr(
            [&](const ExprPtr &Child) { Work.push_back(Child.get()); });
      }
    });
    if (!Safe)
      return false;
  }
  return true;
}

std::optional<int64_t> privateFrameOffset(const ExprPtr &Address,
                                          uint16_t Bytes, const HighFunc &Func,
                                          Arch Architecture, size_t &Budget,
                                          const VarKeyMap<ExprPtr> &Aliases) {
  if (!Bytes)
    return std::nullopt;
  const auto Offset = high_detail::frameAddressOffset(
      Address, Func, Architecture, Budget, 0, &Aliases);
  if (!Offset || *Offset >= 0 ||
      *Offset < -static_cast<int64_t>(Func.FrameSize) ||
      uint64_t(Bytes) > uint64_t(-*Offset))
    return std::nullopt;
  return Offset;
}

ExprPtr replaceFrameLoads(const ExprPtr &Root,
                          const std::map<int64_t, StoredFrameValue> &Stores,
                          const HighFunc &Func, Arch Architecture,
                          size_t &Budget, const VarKeyMap<ExprPtr> &Aliases) {
  if (!Root)
    return Root;

  struct Item {
    ExprPtr Expr;
    bool ChildrenReady = false;
  };
  std::vector<Item> Work{{Root, false}};
  std::unordered_map<const HighExpr *, ExprPtr> Rebuilt;
  std::unordered_set<const HighExpr *> Active;

  while (!Work.empty()) {
    Item Current = std::move(Work.back());
    Work.pop_back();
    const ExprPtr &Expr = Current.Expr;
    if (!Expr || Rebuilt.count(Expr.get()))
      continue;

    if (!Current.ChildrenReady) {
      if (!Active.insert(Expr.get()).second) {
        Rebuilt.emplace(Expr.get(), Expr);
        continue;
      }
      if (Expr->Kind == ExprKind::Load && Expr->Type &&
          Expr->Operands.size() == 1 &&
          Expr->MemoryOrdering == NdMemoryOrdering::None &&
          Expr->MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        const auto Offset =
            privateFrameOffset(Expr->Operands[0], Expr->Type->Size, Func,
                               Architecture, Budget, Aliases);
        const auto It = Offset ? Stores.find(*Offset) : Stores.end();
        if (It != Stores.end() && It->second.Bytes == Expr->Type->Size &&
            sameIntegerType(It->second.Type, Expr->Type) &&
            It->second.Nodes <= Budget) {
          Budget -= It->second.Nodes;
          Rebuilt.emplace(Expr.get(), It->second.Value);
        } else {
          Rebuilt.emplace(Expr.get(), Expr);
        }
        Active.erase(Expr.get());
        continue;
      }
      Work.push_back({Expr, true});
      Expr->forEachChildExpr([&](const ExprPtr &Child) {
        if (Child && !Rebuilt.count(Child.get()))
          Work.push_back({Child, false});
      });
      continue;
    }

    Active.erase(Expr.get());
    bool Changed = false;
    ExprPtr Result = Expr;
    for (const ExprPtr &Child : Expr->Operands) {
      if (!Child)
        continue;
      auto It = Rebuilt.find(Child.get());
      if (It != Rebuilt.end())
        Changed |= It->second != Child;
    }
    if (Expr->IndirectTarget) {
      auto It = Rebuilt.find(Expr->IndirectTarget.get());
      if (It != Rebuilt.end())
        Changed |= It->second != Expr->IndirectTarget;
    }
    if (Changed) {
      Result = std::make_shared<HighExpr>(*Expr);
      for (ExprPtr &Child : Result->Operands)
        if (Child)
          if (auto It = Rebuilt.find(Child.get()); It != Rebuilt.end())
            Child = It->second;
      if (Result->IndirectTarget)
        if (auto It = Rebuilt.find(Result->IndirectTarget.get());
            It != Rebuilt.end())
          Result->IndirectTarget = It->second;
    }
    Rebuilt.emplace(Expr.get(), std::move(Result));
  }

  const auto It = Rebuilt.find(Root.get());
  return It == Rebuilt.end() ? Root : It->second;
}

void forgetOverlapping(std::map<int64_t, StoredFrameValue> &Stores,
                       int64_t Offset, uint16_t Bytes) {
  for (auto It = Stores.begin(); It != Stores.end();) {
    if (rangesOverlap(It->second.Offset, It->second.Bytes, Offset, Bytes))
      It = Stores.erase(It);
    else
      ++It;
  }
}

void recordFrameStore(const ExprPtr &Address, const ExprPtr &Value,
                      std::map<int64_t, StoredFrameValue> &Stores,
                      const HighFunc &Func, Arch Architecture, size_t &Budget,
                      const VarKeyMap<ExprPtr> &Aliases) {
  const uint16_t Bytes = Value && Value->Type ? Value->Type->Size : uint16_t(0);
  const auto Offset =
      privateFrameOffset(Address, Bytes, Func, Architecture, Budget, Aliases);
  if (!Offset) {
    Stores.clear();
    return;
  }
  forgetOverlapping(Stores, *Offset, Bytes);
  if (!isForwardableInteger(Value ? Value->Type : nullptr))
    return;

  StoredFrameValue Stored;
  Stored.Offset = *Offset;
  Stored.Bytes = Bytes;
  Stored.Type = Value->Type;
  Stored.Value = Value;
  if (!safeFrameValue(Value, Stored.Dependencies, Stored.Nodes))
    return;
  Stores.emplace(*Offset, std::move(Stored));
}

void invalidateWrittenVariable(std::map<int64_t, StoredFrameValue> &Stores,
                               const MedVar &Variable) {
  const VarKey Key = VK(Variable);
  for (auto It = Stores.begin(); It != Stores.end();) {
    if (It->second.Dependencies.count(Key))
      It = Stores.erase(It);
    else
      ++It;
  }
}

} // namespace

void forwardPrivateFrameLoads(HighFunc &Func, Arch Architecture) {
  // This proof currently covers the x86-64 frame-address forms exercised by
  // native formats. Keep other architectures on their existing path until
  // their ABI-specific stack lowering has equivalent coverage.
  if (Architecture != Arch::X64 || !isStraightLinePrivateFrameFunction(Func))
    return;

  const uint16_t PointerBytes = getTargetRegInfo(Architecture).PointerSize;
  size_t Budget = kFrameWalkBudget;
  VarKeyMap<unsigned> DefinitionCounts;
  for (const HighStmt &Stmt : Func.Body)
    if (Stmt.Kind == StmtKind::Assign && Stmt.Dst &&
        Stmt.Dst->Kind == ExprKind::Var)
      ++DefinitionCounts[VK(Stmt.Dst->Var)];

  VarKeyMap<ExprPtr> Aliases;
  for (const HighStmt &Stmt : Func.Body) {
    if (Stmt.Kind != StmtKind::Assign || !Stmt.Dst || !Stmt.Val ||
        Stmt.Dst->Kind != ExprKind::Var || !Stmt.Dst->Type ||
        Stmt.Dst->Type->Size != PointerBytes ||
        Stmt.Dst->Var.Size != PointerBytes ||
        DefinitionCounts[VK(Stmt.Dst->Var)] != 1)
      break;
    if (!high_detail::frameAddressOffset(Stmt.Val, Func, Architecture, Budget,
                                         0, &Aliases))
      break;
    Aliases.emplace(VK(Stmt.Dst->Var), Stmt.Val);
  }

  std::map<int64_t, StoredFrameValue> Stores;
  for (HighStmt &Stmt : Func.Body) {
    const auto Rewrite = [&](ExprPtr &Expr) {
      size_t ExpansionBudget = kMaxForwardExpansionNodes;
      Expr = replaceFrameLoads(Expr, Stores, Func, Architecture,
                               ExpansionBudget, Aliases);
    };
    forEachRhsExpr(Stmt, Rewrite);

    if (Stmt.Kind == StmtKind::Store) {
      recordFrameStore(Stmt.StoreAddr, Stmt.StoreVal, Stores, Func,
                       Architecture, Budget, Aliases);
    } else if (Stmt.Kind == StmtKind::Assign && Stmt.Dst &&
               Stmt.Dst->Kind == ExprKind::Load &&
               Stmt.Dst->Operands.size() == 1) {
      recordFrameStore(Stmt.Dst->Operands[0], Stmt.Val, Stores, Func,
                       Architecture, Budget, Aliases);
    } else if (Stmt.Kind == StmtKind::Assign && Stmt.Dst &&
               Stmt.Dst->Kind == ExprKind::Var) {
      invalidateWrittenVariable(Stores, Stmt.Dst->Var);
    }
  }
}

} // namespace neverd
