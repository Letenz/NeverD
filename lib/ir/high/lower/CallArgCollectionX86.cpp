//===- CallArgCollectionX86.cpp - x86 / x86-64 call-argument ABI ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86 and x86-64 stack-argument recovery.  Both use the shared spilled-store
/// scan (slot width is TRI.PointerSize).  i386 cdecl/stdcall additionally
/// pushes each argument, so every STORE looks like slot 0; walk those pushes
/// backward (last push is arg0).
///
//===----------------------------------------------------------------------===//

#include "CallArgCollectionDetail.h"

#include "neverd/ir/TargetRegInfo.h"

namespace neverd {
namespace call_args_detail {

void collectCallArgsX86(const CallArgScan &Scan, std::vector<ExprPtr> &Found,
                        std::vector<ExprPtr> &Args) {
  collectSpilledStackArgs(Scan, Found);
  for (int K = 0; K < Scan.MaxArgs; ++K) {
    if (!Found[K])
      break;
    Args.push_back(Found[K]);
  }
  if (Scan.TheArch != Arch::X86)
    return;

  const auto &Ops = *Scan.Ops;
  std::vector<ExprPtr> Pushed;
  for (int J = static_cast<int>(Scan.CallIdx) - 1; J >= 0; --J) {
    const MedOp &Prev = Ops[J];
    if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
        Prev.Opcode == NdOp::INTRINSIC)
      break;
    if (Prev.Opcode != NdOp::STORE || Prev.NumInputs < 2 ||
        Prev.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      continue;
    if (Scan.IsCalleeSave(Prev.Inputs[1]))
      continue;
    Pushed.push_back(Scan.ToExpr(Prev.Inputs[1]));
    if (static_cast<int>(Pushed.size()) == Scan.MaxArgs)
      break;
  }
  if (Pushed.size() > Args.size())
    Args = std::move(Pushed);
}

} // namespace call_args_detail
} // namespace neverd
