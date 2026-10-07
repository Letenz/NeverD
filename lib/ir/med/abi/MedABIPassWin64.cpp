//===- MedABIPassWin64.cpp - Microsoft x64 call-ABI recovery steps --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The Microsoft x64 steps of recoverCallAbi.  MSVC splits a call from its
/// setup at EH state changes and `cmp/jnz` boundaries, so a call first in its
/// block can have its home-area stores and register writes in a predecessor;
/// it passes a member function's `this` in RCX and a returned aggregate's
/// buffer in RDX after it.  Each step recovers one of those shapes; generic
/// recovery runs them through Win64AbiCallPolicy.
///
//===----------------------------------------------------------------------===//

#include "MedABIPassDetail.h"

#include "neverd/ir/TargetRegInfo.h"

namespace neverd {

namespace {

const MedBlock *blockById(const MedFunc &Func, int Id) {
  for (const auto &Cand : Func.Blocks)
    if (Cand.Id == Id)
      return &Cand;
  return nullptr;
}

/// The call-relative offset of a store at the end of a predecessor.  The
/// outgoing `[rsp+20h]` address is an INT_ADD there that the call-SP map may
/// not hold yet; walk the trailing window the same way HighIR
/// collectSpilledStackArgs does.
std::optional<int64_t> predecessorStoreOffset(const AbiCallContext &C,
                                              const MedBlock &Pred,
                                              int StoreIdx) {
  const MedOp &Store = Pred.Ops[static_cast<size_t>(StoreIdx)];
  if (auto Rel = C.CallStackOffset(Store.Inputs[0]))
    return Rel;
  const MedVar &AddrVar = Store.Inputs[0];
  if (AddrVar.Kind == MedVar::Reg && AddrVar.RegOff == C.TRI.StackPointer)
    return 0;
  for (int K = StoreIdx - 1; K >= 0; --K) {
    const MedOp &DefOp = Pred.Ops[static_cast<size_t>(K)];
    if (DefOp.Opcode == NdOp::CALL || DefOp.Opcode == NdOp::INDIR_CALL ||
        DefOp.Opcode == NdOp::INTRINSIC)
      break;
    if (DefOp.Output.Id != AddrVar.Id || DefOp.Output.SSAVer != AddrVar.SSAVer)
      continue;
    if (DefOp.Opcode != NdOp::INT_ADD || DefOp.NumInputs < 2)
      break;
    bool HasSP = false;
    int64_t ConstOff = -1;
    for (uint8_t KI = 0; KI < DefOp.NumInputs; ++KI) {
      if (DefOp.Inputs[KI].Kind == MedVar::Reg &&
          DefOp.Inputs[KI].RegOff == C.TRI.StackPointer)
        HasSP = true;
      if (DefOp.Inputs[KI].isConst())
        ConstOff = static_cast<int64_t>(DefOp.Inputs[KI].ConstVal);
    }
    if (HasSP && ConstOff >= 0)
      return ConstOff;
    break;
  }
  return std::nullopt;
}

/// Call-only block: `mov [rsp+20h], esi` sits in the predecessor after the
/// last helper CALL.  That store proves every parameter register is live at
/// the call (`Concatenate` dest + two string/length pairs).
void scanPredecessorStackArgs(const AbiCallContext &C, bool &HasStackArg,
                              bool &HasStackArgAtCallSP) {
  for (int PredId : C.Blk.Preds) {
    const MedBlock *Pred = blockById(C.Func, PredId);
    if (!Pred)
      continue;
    for (int J = static_cast<int>(Pred->Ops.size()) - 1; J >= 0; --J) {
      const MedOp &Prev = Pred->Ops[static_cast<size_t>(J)];
      if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
          Prev.Opcode == NdOp::INTRINSIC)
        break;
      if (Prev.Opcode != NdOp::STORE || Prev.NumInputs < 2 ||
          Prev.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        continue;
      if (auto Rel = predecessorStoreOffset(C, *Pred, J);
          Rel && *Rel >= C.Layout.CallStackBase) {
        HasStackArg = true;
        if (*Rel == C.Layout.CallStackBase &&
            !(Prev.Inputs[1].Kind == MedVar::Reg &&
              C.TRI.isFrameOrLinkReg(Prev.Inputs[1].RegOff)))
          HasStackArgAtCallSP = true;
        break;
      }
    }
  }
}

/// The home-area stores at the end of the predecessor of a call-only block
/// are its stack arguments.
void takePredecessorStackArgs(AbiCallContext &C) {
  const int PredSlotSize = C.Layout.SlotBytes;
  const int PredStackBase = static_cast<int>(C.Layout.Registers.size());
  for (int PredId : C.Blk.Preds) {
    const MedBlock *Pred = blockById(C.Func, PredId);
    if (!Pred)
      continue;
    for (int J = static_cast<int>(Pred->Ops.size()) - 1; J >= 0; --J) {
      const MedOp &Prev = Pred->Ops[static_cast<size_t>(J)];
      if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
          Prev.Opcode == NdOp::INTRINSIC)
        break;
      if (Prev.Opcode != NdOp::STORE || Prev.NumInputs < 2 ||
          Prev.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        continue;
      if (Prev.Inputs[1].Kind == MedVar::Reg &&
          C.TRI.isCalleeSaveReg(Prev.Inputs[1].RegOff) &&
          Prev.Inputs[1].SSAVer == 0)
        continue;
      auto Rel = predecessorStoreOffset(C, *Pred, J);
      if (!Rel || *Rel < C.Layout.CallStackBase || PredSlotSize == 0)
        continue;
      const int64_t SlotOff = *Rel - C.Layout.CallStackBase;
      if (SlotOff % PredSlotSize != 0)
        continue;
      const int SlotIdx =
          PredStackBase + static_cast<int>(SlotOff / PredSlotSize);
      if (SlotIdx >= 0 && SlotIdx < C.MaxArgs && !C.FoundMask[SlotIdx]) {
        C.Found[SlotIdx] = Prev.Inputs[1];
        C.FoundMask[SlotIdx] = true;
        C.FromStackScan[SlotIdx] = true;
      }
    }
  }
}

/// Intervening Win64 thiscall clobbers rcx.  The unique predecessor's
/// `if (p)` pointer is the callee this, not a reaching pred rcx / live-in
/// parent this.  An in-block `mov rcx` after the helper still wins.
void resolveFirstArgAfterCall(AbiCallContext &C, bool &Arg0FromInBlock) {
  if (C.FoundMask[0]) {
    for (const MedCallClobber &Clobber : C.Func.CallClobbers) {
      if (Clobber.Value.Kind == C.Found[0].Kind &&
          Clobber.Value.Id == C.Found[0].Id &&
          Clobber.Value.SSAVer == C.Found[0].SSAVer &&
          Clobber.Value.RegOff == C.Found[0].RegOff) {
        Arg0FromInBlock = false;
        break;
      }
    }
  }
  if (Arg0FromInBlock)
    return;
  bool HasInterveningCall = false;
  for (int J = 0; J < C.CallIdx; ++J) {
    const NdOp PrevOp = C.Blk.Ops[static_cast<size_t>(J)].Opcode;
    if (PrevOp == NdOp::CALL || PrevOp == NdOp::INDIR_CALL) {
      HasInterveningCall = true;
      break;
    }
  }
  if (HasInterveningCall)
    if (auto Guard = uniquePredNonNullGuard(C.Func, C.Blk)) {
      C.Found[0] = *Guard;
      C.FoundMask[0] = true;
    }
}

/// Win64 IAT call with no stack arg: rcx/rdx written in the predecessor are
/// the real arguments (`cstr(&name)`, `assign(&dst, &src)`).  Do not use the
/// function's incoming this when nothing in the function wrote that
/// register.
void takeIndirectCallRegisters(AbiCallContext &C) {
  const int NumIntParamRegs = static_cast<int>(C.Layout.Registers.size());
  for (int K = 0; K < NumIntParamRegs && K < C.MaxArgs; ++K) {
    if (C.FoundMask[K])
      continue;
    if (K > 0 && !C.FoundMask[K - 1])
      break;
    bool FoundDef = false;
    auto V =
        findReachingArgReg(C.Func, C.TRI, C.TheArch, C.Blk.Id, K, C.Layout,
                           /*AllowUnknownLiveIn=*/false, nullptr, &FoundDef);
    if (!V || !FoundDef)
      break;
    C.Found[K] = *V;
    C.FoundMask[K] = true;
  }
}

/// A virtual call's object is arg0; when this function already has an rdx
/// param, the result buffer it passes on is arg1.
void takeVirtualCallResultBuffer(AbiCallContext &C) {
  if (!C.FoundMask[0] || C.FoundMask[1] || C.Layout.Registers.size() <= 1)
    return;
  const uint64_t SretOff = C.Layout.Registers[1];
  bool HaveSretParam = false;
  for (const auto &P : C.Func.Params)
    if ((P.Kind == MedVar::Reg || P.Kind == MedVar::Param) &&
        P.RegOff == SretOff)
      HaveSretParam = true;
  if (HaveSretParam)
    if (auto V =
            findReachingArgReg(C.Func, C.TRI, C.TheArch, C.Blk.Id, 1, C.Layout,
                               /*AllowUnknownLiveIn=*/false, nullptr)) {
      C.Found[1] = *V;
      C.FoundMask[1] = true;
    }
}

} // namespace

extern const AbiCallPolicy Win64AbiCallPolicy;
const AbiCallPolicy Win64AbiCallPolicy = {
    .TheArch = Arch::X64,
    .Format = BinaryFormat::COFF,
    .ScanPredecessorStackArgs = scanPredecessorStackArgs,
    .TakePredecessorStackArgs = takePredecessorStackArgs,
    .ResolveFirstArgAfterCall = resolveFirstArgAfterCall,
    .TakeIndirectCallRegisters = takeIndirectCallRegisters,
    .TakeVirtualCallResultBuffer = takeVirtualCallResultBuffer,
};

} // namespace neverd
