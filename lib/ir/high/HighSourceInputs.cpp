#include "neverd/ir/high/HighSourceInputs.h"

namespace neverd {
namespace {
bool plain(const HighExpr &Expression) {
  return Expression.IntrinsicId == Intrinsic::None &&
         Expression.IntrinsicOutputs.empty() &&
         Expression.MemoryOrdering == NdMemoryOrdering::None &&
         Expression.MemoryAddressSpace == NdMemoryAddressSpace::Default;
}

bool pureEntryValue(const ExprPtr &Expression, unsigned Depth = 0) {
  if (!Expression || Depth > 16 || !plain(*Expression) || !Expression->Type ||
      Expression->Type->Kind != NdTypeKind::Int || !Expression->Type->Size ||
      Expression->Type->Size > 8)
    return false;
  if (Expression->Kind == ExprKind::Var || Expression->Kind == ExprKind::Const)
    return Expression->Operands.empty();
  if (Expression->Kind == ExprKind::Cast && Expression->Operands.size() == 1)
    return pureEntryValue(Expression->Operands[0], Depth + 1);
  return Expression->Kind == ExprKind::BinOp &&
         (Expression->Op == NdOp::INT_ADD || Expression->Op == NdOp::INT_SUB) &&
         Expression->Operands.size() == 2 &&
         pureEntryValue(Expression->Operands[0], Depth + 1) &&
         pureEntryValue(Expression->Operands[1], Depth + 1);
}
} // namespace

TypeRef entryScalarLoadInput(const HighFunc &Function, unsigned Parameter) {
  if (Parameter >= Function.Params.size() || !Function.Params[Parameter].Type ||
      Function.Params[Parameter].Type->Kind != NdTypeKind::Ptr ||
      Function.Params[Parameter].Type->Size != 8 ||
      Function.Body.size() > 256 || Function.StructuredExceptionRegions ||
      Function.UnstructuredExceptionRegions)
    return {};
  const HighExpr *Load = nullptr;
  bool AtEntry = true;
  size_t Budget = 4096;
  unsigned Uses = 0;
  const auto Scan = [&](auto &&Self, const ExprPtr &Expression,
                        unsigned Depth) -> bool {
    if (!Expression || !Budget || Depth > 32)
      return false;
    --Budget;
    if (Expression->Kind == ExprKind::Var || Expression->Kind == ExprKind::Phi)
      if (Expression->Var.Kind == MedVar::Param &&
          Expression->Var.Id == static_cast<int>(Parameter) && ++Uses > 1)
        return false;
    for (const auto &Output : Expression->IntrinsicOutputs)
      if (Output.Kind == MedVar::Param &&
          Output.Id == static_cast<int>(Parameter))
        return false;
    for (const auto &Operand : Expression->Operands)
      if (!Self(Self, Operand, Depth + 1))
        return false;
    return true;
  };
  for (const auto &Statement : Function.Body) {
    // There are no alternate entries or reexecuted loads in this subset.
    // Broader control flow needs a proof over HighSourceFlow, not lexical
    // order.
    if (!Statement.Body.empty() || !Statement.ElseBody.empty() ||
        !Statement.DefaultBody.empty() || !Statement.Cases.empty() ||
        !Statement.EHClauseBodies.empty() ||
        (Statement.Kind != StmtKind::Assign &&
         Statement.Kind != StmtKind::Store &&
         Statement.Kind != StmtKind::Call &&
         Statement.Kind != StmtKind::ExprStmt &&
         Statement.Kind != StmtKind::Return && Statement.Kind != StmtKind::Nop))
      return {};
    bool ValidUses = true;
    forEachExpr(Statement, [&](const ExprPtr &Expression) {
      ValidUses &= Scan(Scan, Expression, 0);
    });
    if (!ValidUses)
      return {};
    if (!AtEntry)
      continue;
    if (Statement.MemoryOrdering != NdMemoryOrdering::None ||
        Statement.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return {};
    if (Statement.Kind == StmtKind::Nop) {
      bool Empty = true;
      forEachExpr(Statement, [&](const ExprPtr &) { Empty = false; });
      if (!Empty)
        return {};
      continue;
    }
    if (Statement.Kind != StmtKind::Assign || !Statement.Dst ||
        Statement.Dst->Kind != ExprKind::Var || !plain(*Statement.Dst) ||
        (Statement.Dst->Var.Kind != MedVar::Temp &&
         Statement.Dst->Var.Kind != MedVar::Reg) ||
        !Statement.Dst->Operands.empty() || Statement.Cond ||
        Statement.RetVal || Statement.StoreAddr || Statement.StoreVal ||
        Statement.CallExpr || Statement.SwitchExpr)
      return {};
    auto Value = Statement.Kind == StmtKind::Assign ? Statement.Val : nullptr;
    while (Value && Value->Kind == ExprKind::Cast && plain(*Value) &&
           Value->Type && Value->Type->Kind == NdTypeKind::Int &&
           Value->Operands.size() == 1)
      Value = Value->Operands[0];
    if (Value && Value->Kind == ExprKind::Load && plain(*Value) &&
        Value->Type && Value->Type->Kind == NdTypeKind::Int &&
        (Value->Type->Size == 4 || Value->Type->Size == 8) &&
        Value->Operands.size() == 1 && Value->Operands[0]) {
      auto Address = Value->Operands[0];
      while (Address && Address->Kind == ExprKind::Cast && plain(*Address) &&
             Address->Type && Address->Type->Size == 8 &&
             (Address->Type->Kind == NdTypeKind::Int ||
              Address->Type->Kind == NdTypeKind::Ptr) &&
             Address->Operands.size() == 1)
        Address = Address->Operands[0];
      if (!Address)
        return {};
      const auto &Pointer = *Address;
      if (Pointer.Kind == ExprKind::Var && plain(Pointer) &&
          Pointer.Operands.empty() && Pointer.Var.Kind == MedVar::Param &&
          Pointer.Var.Id == static_cast<int>(Parameter) &&
          !Pointer.Var.SSAVer && Pointer.Var.RenameTag < 0 && Pointer.Type &&
          (Pointer.Type->Kind == NdTypeKind::Ptr ||
           Pointer.Type->Kind == NdTypeKind::Int) &&
          Pointer.Type->Size == 8 && Pointer.Var.Size == 8) {
        Load = Value.get();
        AtEntry = false;
        continue;
      }
    }
    if (Statement.Kind != StmtKind::Assign || !Statement.Dst ||
        Statement.Dst->Kind != ExprKind::Var || !pureEntryValue(Statement.Val))
      return {};
  }
  return Load && Uses == 1 ? Load->Type : TypeRef{};
}
} // namespace neverd
