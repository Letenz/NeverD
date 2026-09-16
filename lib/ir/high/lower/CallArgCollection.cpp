//===- CallArgCollection.cpp - Call argument collection
//--------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Collects function call arguments by scanning backward from the call site
/// for register writes and stack stores that match the target ABI.  Shared
/// register/live-in recovery lives here; ISA stack-argument quirks live in
/// CallArgCollectionX86.cpp, CallArgCollectionARM.cpp, and
/// CallArgCollectionAArch64.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/Limits.h"
#include "neverd/loader/BinaryImage.h"

#include "CallArgCollectionDetail.h"

#include <functional>
#include <set>

namespace neverd {

namespace call_args_detail {

void collectSpilledStackArgs(const CallArgScan &Scan,
                             std::vector<ExprPtr> &Found) {
  const auto &Ops = *Scan.Ops;
  const TargetRegInfo &TRI = *Scan.TRI;
  const int64_t SlotBytes = static_cast<int64_t>(TRI.PointerSize);
  const int StoreScanStart = std::max(
      0, static_cast<int>(Scan.CallIdx) - limits::kCallArgStoreScanWindow);

  for (int J = static_cast<int>(Scan.CallIdx) - 1; J >= StoreScanStart; --J) {
    const MedOp &Prev = Ops[J];
    if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
        Prev.Opcode == NdOp::INTRINSIC)
      break;
    if (Prev.Opcode != NdOp::STORE || Prev.NumInputs < 2 ||
        Prev.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      continue;
    if (Scan.IsCalleeSave(Prev.Inputs[1]))
      continue;

    const MedVar &AddrVar = Prev.Inputs[0];
    int64_t StackOff = -1;

    if (AddrVar.Kind == MedVar::Reg && AddrVar.RegOff == Scan.SpRegOff)
      StackOff = 0;

    if (StackOff < 0 && !AddrVar.isConst()) {
      for (int K = J - 1; K >= 0; --K) {
        const MedOp &DefOp = Ops[K];
        if (DefOp.Output.Id != AddrVar.Id ||
            DefOp.Output.SSAVer != AddrVar.SSAVer)
          continue;
        if (DefOp.Opcode == NdOp::INT_ADD && DefOp.NumInputs >= 2) {
          bool HasSP = false;
          int64_t ConstOff = -1;
          for (uint8_t KI = 0; KI < DefOp.NumInputs; ++KI) {
            if (DefOp.Inputs[KI].Kind == MedVar::Reg &&
                DefOp.Inputs[KI].RegOff == Scan.SpRegOff)
              HasSP = true;
            if (DefOp.Inputs[KI].isConst())
              ConstOff = static_cast<int64_t>(DefOp.Inputs[KI].ConstVal);
          }
          if (HasSP && ConstOff >= 0)
            StackOff = ConstOff;
        }
        break;
      }
    }

    if (StackOff < 0 || StackOff >= Scan.MaxArgs * SlotBytes)
      continue;
    if (SlotBytes == 0 || StackOff % SlotBytes != 0)
      continue;

    const int ArgPos =
        Scan.FirstStackSlot + static_cast<int>(StackOff / SlotBytes);
    if (ArgPos >= 0 && ArgPos < Scan.MaxArgs && !Found[ArgPos])
      Found[ArgPos] = Scan.ToExpr(Prev.Inputs[1]);
  }
}

} // namespace call_args_detail

std::vector<ExprPtr>
MedToHighConverter::collectCallArgs(const MedBlock &CurBlock, size_t CallIdx) {
  const auto &Ops = CurBlock.Ops;
  if (CallIdx < Ops.size() && Ops[CallIdx].SourceCallHint) {
    const auto &Call = Ops[CallIdx];
    const auto &Signature = Call.SourceCallHint->Signature;
    const auto Bindings = sourceABIParameters(Signature);
    const size_t Count = Signature.HasExplicitABI ? Bindings.size()
                                                  : Signature.Parameters.size();
    if (!Signature.HasExplicitABI &&
        std::any_of(Signature.Parameters.begin(), Signature.Parameters.end(),
                    [](const auto &P) { return !P.Components.empty(); }))
      return {};
    if (Count > 64 || Call.NumInputs != Count + 1)
      return {};
    std::vector<ExprPtr> Arguments;
    Arguments.reserve(Count);
    size_t Index = 0;
    for (const auto &P : Signature.Parameters) {
      if (P.Components.empty()) {
        Arguments.push_back(medvarToExpr(Call.Inputs[++Index]));
      } else {
        std::vector<ExprPtr> Leaves;
        for (const auto &Member : sourceAggregateMembers(P.Type))
          Leaves.push_back(
              sourceScalarValue(Call.Inputs[++Index], Member.Type));
        Arguments.push_back(HighExpr::makeRecord(P.Type, std::move(Leaves)));
      }
    }
    return Arguments;
  }

  const int MaxArgs = limits::kMaxRecoveredCallArgs;
  std::vector<ExprPtr> Found(MaxArgs);

  const auto &TRI = getTargetRegInfo(TargetArch);
  const BinaryFormat Format = Image ? Image->Format : BinaryFormat::Unknown;
  const auto ParamRegs = TRI.integerParamRegs(Format);
  const uint64_t SpRegOff = TRI.StackPointer;

  auto IsCalleeSave = [&TRI](const MedVar &V) -> bool {
    return V.Kind == MedVar::Reg && TRI.isCalleeSaveReg(V.RegOff);
  };

  bool ReachedBlockStart = true;
  for (int J = static_cast<int>(CallIdx) - 1; J >= 0; --J) {
    const MedOp &Prev = Ops[J];
    if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
        Prev.Opcode == NdOp::INTRINSIC) {
      ReachedBlockStart = false;
      break;
    }
    if (Prev.Opcode == NdOp::COPY && Prev.NumInputs >= 1 &&
        Prev.Output.Kind == MedVar::Reg && Prev.Inputs[0].Kind == MedVar::Reg &&
        Prev.Output.RegOff == Prev.Inputs[0].RegOff &&
        Prev.Output.Size == Prev.Inputs[0].Size)
      continue;
    if (Prev.Output.Kind == MedVar::Reg && Prev.Output.Size > 0) {
      const int ArgIdx = regToArgIdx(Prev.Output.RegOff);
      if (ArgIdx >= 0 && ArgIdx < MaxArgs && !Found[ArgIdx]) {
        if (Prev.Opcode == NdOp::COPY && Prev.NumInputs >= 1)
          Found[ArgIdx] = medvarToExpr(Prev.Inputs[0]);
        else
          Found[ArgIdx] = medOpToExpr(Prev);
      }
    }
  }

  if (ReachedBlockStart) {
    int MaxRegArg = -1;
    for (int K = 0; K < MaxArgs; ++K)
      if (Found[K])
        MaxRegArg = K;
    for (int K = 0; K < MaxRegArg; ++K) {
      if (Found[K] || K >= static_cast<int>(ParamRegs.size()))
        continue;
      MedVar LiveIn;
      if (reachingRegAtBlockEntry(CurBlock, ParamRegs[K], LiveIn))
        Found[K] = medvarToExpr(LiveIn);
    }
  }

  int FirstStackSlot = 0;
  for (int K = 0; K < MaxArgs; ++K) {
    if (Found[K])
      FirstStackSlot = K + 1;
    else
      break;
  }

  call_args_detail::CallArgScan Scan;
  Scan.Ops = &Ops;
  Scan.CallIdx = CallIdx;
  Scan.SpRegOff = SpRegOff;
  Scan.TRI = &TRI;
  Scan.Image = Image;
  Scan.TheArch = TargetArch;
  Scan.MaxArgs = MaxArgs;
  Scan.FirstStackSlot = FirstStackSlot;
  auto ToExpr = [this](const MedVar &V) { return medvarToExpr(V); };
  Scan.ToExpr = ToExpr;
  Scan.IsCalleeSave = IsCalleeSave;

  std::vector<ExprPtr> Args;
  switch (TargetArch) {
  case Arch::X86:
  case Arch::X64:
    call_args_detail::collectCallArgsX86(Scan, Found, Args);
    break;
  case Arch::ARM:
    call_args_detail::collectCallArgsARM(Scan, Found, Args);
    break;
  case Arch::AArch64:
    call_args_detail::collectCallArgsAArch64(Scan, Found, Args);
    break;
  default:
    call_args_detail::collectSpilledStackArgs(Scan, Found);
    break;
  }

  if (Args.empty()) {
    for (int K = 0; K < MaxArgs; ++K) {
      if (!Found[K])
        break;
      Args.push_back(Found[K]);
    }
  }
  return Args;
}

bool MedToHighConverter::reachingRegAtBlockEntry(const MedBlock &B,
                                                 uint64_t RegOff,
                                                 MedVar &Out) const {
  if (!CurMed)
    return false;

  auto blockById = [&](int Id) -> const MedBlock * {
    for (const auto &Blk : CurMed->Blocks)
      if (Blk.Id == Id)
        return &Blk;
    return nullptr;
  };

  std::set<int> Visited;
  std::function<bool(const MedBlock &, MedVar &)> entryOf;
  std::function<bool(const MedBlock &, MedVar &)> exitOf;
  entryOf = [&](const MedBlock &Blk, MedVar &R) -> bool {
    if (!Visited.insert(Blk.Id).second)
      return false;
    for (const auto &Phi : Blk.Phis)
      if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == RegOff &&
          Phi.Output.Size > 0) {
        R = Phi.Output;
        return true;
      }
    for (int P : Blk.Preds)
      if (const MedBlock *PB = blockById(P))
        if (exitOf(*PB, R))
          return true;
    return false;
  };
  exitOf = [&](const MedBlock &Blk, MedVar &R) -> bool {
    for (auto It = Blk.Ops.rbegin(); It != Blk.Ops.rend(); ++It)
      if (It->Output.Kind == MedVar::Reg && It->Output.RegOff == RegOff &&
          It->Output.Size > 0) {
        R = It->Output;
        return true;
      }
    return entryOf(Blk, R);
  };

  return entryOf(B, Out);
}

int MedToHighConverter::regToArgIdx(uint64_t RegOff) const {
  const bool IsWin64 = TargetArch == Arch::X64 && Image &&
                       Image->Format == BinaryFormat::COFF;
  return getTargetRegInfo(TargetArch).regToArgIdx(RegOff, IsWin64);
}

} // namespace neverd
