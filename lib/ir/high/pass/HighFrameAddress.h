#ifndef NEVERD_IR_HIGH_FRAMEADDRESS_H
#define NEVERD_IR_HIGH_FRAMEADDRESS_H
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/Support/MathExtras.h"
namespace neverd::high_detail {
/// A bounded arithmetic offset from the synthetic entry stack pointer.
/// The caller must separately check the access width and private frame bounds.
inline std::optional<int64_t>
frameAddressOffset(const ExprPtr &E, const HighFunc &Func, Arch Architecture,
                   size_t &Budget, unsigned Depth = 0) {
  const auto &TRI = getTargetRegInfo(Architecture);
  if (!E || !E->Type || E->Type->Size != TRI.PointerSize ||
      E->MemoryOrdering != NdMemoryOrdering::None ||
      E->MemoryAddressSpace != NdMemoryAddressSpace::Default || Depth > 32 ||
      !Budget)
    return std::nullopt;
  --Budget;
  if (E->Kind == ExprKind::Var && E->Var.Size == TRI.PointerSize &&
      isSyntheticEntryStackPointer(E->Var, Func, Architecture))
    return 0;
  if (E->Kind != ExprKind::BinOp || E->Operands.size() != 2 ||
      !E->Operands[1] || E->Operands[1]->Kind != ExprKind::Const ||
      !E->Operands[1]->Type || !E->Operands[1]->Type->Size ||
      E->Operands[1]->Type->Size > TRI.PointerSize ||
      (E->Operands[1]->Type->Size < 8 &&
       E->Operands[1]->ConstVal >=
           (UINT64_C(1) << (E->Operands[1]->Type->Size * 8))) ||
      (E->Op != NdOp::INT_ADD && E->Op != NdOp::INT_SUB))
    return std::nullopt;
  const auto Base =
      frameAddressOffset(E->Operands[0], Func, Architecture, Budget, Depth + 1);
  if (!Base)
    return std::nullopt;
  const int64_t Delta = TRI.PointerSize == 4
                            ? int64_t(int32_t(E->Operands[1]->ConstVal))
                            : int64_t(E->Operands[1]->ConstVal);
  int64_t Result;
  if (E->Op == NdOp::INT_ADD ? llvm::AddOverflow(*Base, Delta, Result)
                             : llvm::SubOverflow(*Base, Delta, Result))
    return std::nullopt;
  if (TRI.PointerSize == 4 && Result != int64_t(int32_t(Result)))
    return std::nullopt;
  return Result;
}
} // namespace neverd::high_detail
#endif
