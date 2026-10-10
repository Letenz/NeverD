//===- X86RegistrationReturn.cpp - Catch resumes ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"

#include <algorithm>

namespace neverd {

bool MedToHighConverter::lowerX86RegistrationCatchReturn(
    HighFunc &Func, const MedBlock &CurBlock, const MedOp &CurOp,
    const MedFunc &Med) {
  if (TargetArch != Arch::X86 || !Med.ExceptionMetadata ||
      Med.ExceptionMetadata->Encoding != ExceptionEncoding::X86CxxFuncInfo ||
      !Med.ExceptionMetadata->Registration ||
      Med.ExceptionMetadata->Registration->RealignedFrame ||
      Med.ExceptionMetadata->Registration->RegistrationOffset != -12 ||
      !Med.ExceptionMetadata->Cxx || !Med.RegistrationStates ||
      !Med.RegistrationStates->CxxContinuationsComplete ||
      CurOp.NumInputs != 1 || CurOp.Inputs[0].Size != 4 || CurOp.OriginSeq < 0)
    return false;
  const auto *Resume =
      Med.RegistrationStates->cxxContinuation(CurOp.Addr, CurOp.OriginSeq);
  if (!Resume || Resume->EndAddress != CurBlock.EndAddr ||
      Resume->SavedStackOffset > -16 ||
      !std::any_of(Med.Blocks.begin(), Med.Blocks.end(),
                   [&](const auto &Block) {
                     return Block.StartAddr == Resume->TargetVA;
                   }))
    return false;
  const auto Value = forceInlineExpr(medvarToExpr(CurOp.Inputs[0]));
  if (!Value || Value->Kind != ExprKind::Const ||
      Value->ConstVal != Resume->TargetVA)
    return false;
  // Use entry-SP coordinates, as registration runtime roots do. The
  // catch may have overwritten both EBP and the SavedESP cell.
  MedVar EntrySP;
  EntrySP.Kind = MedVar::Reg;
  EntrySP.TheArch = Arch::X86;
  EntrySP.RegOff = getTargetRegInfo(Arch::X86).StackPointer;
  EntrySP.Size = 4;
  auto FrameAddress = [&](int64_t Offset) {
    return HighExpr::makeBinop(
        NdOp::INT_SUB, HighExpr::makeVar(EntrySP),
        HighExpr::makeConst(uint64_t(4 - Offset), 4,
                            ConstantAddressProvenance::Scalar));
  };
  HighStmt Restore;
  Restore.Kind = StmtKind::Store;
  Restore.Addr = CurOp.Addr;
  Restore.StoreAddr = FrameAddress(
      int64_t(*Med.ExceptionMetadata->Registration->RegistrationOffset) - 4);
  Restore.StoreVal = FrameAddress(Resume->SavedStackOffset);
  Func.Body.push_back(std::move(Restore));
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.Addr = CurOp.Addr;
  Jump.GotoTarget = Resume->TargetVA;
  Func.Body.push_back(std::move(Jump));
  return true;
}

} // namespace neverd
