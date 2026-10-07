//===- CFGBuilderInsn.cpp - Instruction classification and rewriting -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-instruction classification and control-flow rewriting: setting the
/// branch/call/return flags from an instruction's lifted ops, recognizing a
/// direct branch to another function or a no-return libc call, and rewriting
/// direct and indirect tail calls into an explicit call + return pair so ABI
/// recovery sees the arguments set up before the branch.
///
/// See CFGBuilder.cpp for the exploration that drives this classification.
///
//===----------------------------------------------------------------------===//

#include "IndirectTailFrame.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/DirectTailCall.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"

namespace neverd {

namespace {

bool isObservableInstructionEffect(NdOp Opcode) {
  switch (Opcode) {
  case NdOp::LOAD:
  case NdOp::STORE:
  case NdOp::ATOMIC_XCHG:
  case NdOp::ATOMIC_ADD:
  case NdOp::ATOMIC_CMPXCHG:
  case NdOp::INTRINSIC:
  case NdOp::INDIR_BR:
  case NdOp::CALL:
  case NdOp::INDIR_CALL:
  case NdOp::RETURN:
    return true;
  default:
    return false;
  }
}

template <typename InstructionRecordT>
bool hasInstructionLocalGuard(const InstructionRecordT &Rec) {
  if (Rec.Mode != InstructionMode::ARM && Rec.Mode != InstructionMode::Thumb)
    return false;
  const va_t NextAddress = Rec.Addr + Rec.Size;
  for (size_t GuardIndex = 0; GuardIndex < Rec.Ops.size(); ++GuardIndex) {
    const LowOp &Guard = Rec.Ops[GuardIndex];
    if (Guard.Opcode != NdOp::COND_BR || Guard.NumInputs < 1 ||
        !Guard.Inputs[0].isConst() || Guard.Inputs[0].Offset != NextAddress)
      continue;
    // A real conditional direct branch is lifted as
    // `COND_BR next,!condition; BRANCH target`.  The trailing BRANCH is the
    // guest control transfer itself, not an instruction-local guard around an
    // observable side effect.  isObservableInstructionEffect intentionally
    // excludes BRANCH so these control-flow guards remain eligible as CFG
    // range guards; predicated loads/stores/calls/returns stay marked.
    for (size_t I = GuardIndex + 1;
         I < Rec.Ops.size() && Rec.Ops[I].Addr == Guard.Addr; ++I)
      if (isObservableInstructionEffect(Rec.Ops[I].Opcode))
        return true;
  }
  return false;
}

} // namespace

//===----------------------------------------------------------------------===//
// convertIndirectTailCalls — model `bx reg`/`br reg`/`jmp *reg` (function
// pointer, not a jump table) as an indirect call + return.
//===----------------------------------------------------------------------===//

void CFGBuilder::convertIndirectTailCalls(LowFunc &Func) {
  if (!CurrentImg)
    return;
  const bool NeedsFrameGuard = CurrentImg->Arch == Arch::AArch64;
  // Coverage is published on LowFunc later in build(), so consult the current
  // decode/lift account before granting a reaching-state condition here.
  const auto RestoredFrames =
      NeedsFrameGuard && DecodeFailureAddresses.empty() &&
              UnsupportedInstructionAddresses.empty() &&
              TruncatedPathAddresses.empty()
          ? restoredAArch64IndirectTailFrames(Func, CurrentImg->Format)
          : std::set<va_t>{};
  bool Changed = false;
  for (auto &[Addr, Rec] : Insns) {
    // A lack of jump-table targets alone cannot establish a tail call. An
    // AArch64 candidate must also restore its incoming SP and link word on
    // every reaching path. Existing table identity/quarantine guards still own
    // dispatch classification; this frame condition supplies no callee ABI.
    if (!Rec.IsBranch || !Rec.IsIndirect || Rec.IsCall || Rec.IsRet ||
        Rec.IsCond || !Rec.JumpTableTargets.empty() ||
        (NeedsFrameGuard && !RestoredFrames.count(Addr)) ||
        EverPublishedJumpTableBranches.count(Addr) ||
        LostValidatedJumpTableBranches.count(Addr) ||
        StackTableEvidenceIncompleteBranches.count(Addr) ||
        IndexDomainEvidenceIncompleteBranches.count(Addr) ||
        IncompleteBranchMarkerEvidenceIncomplete ||
        ((finiteGOTOFFGroupClaimed() || finiteAbsoluteGroupClaimed()) &&
         guardedGroupContains(Addr)) ||
        ValidatedPhysicalJumpTableBranches.count(Addr) ||
        AmbiguousI386GOTPCBranches.count(Addr) ||
        PendingAmbiguousI386GOTPCBranches.count(Addr) ||
        (PreservePotentialJumpTableBranches &&
         PotentialJumpTableBranches.count(Addr)) ||
        (UnsafeJumpTableBranches && UnsafeJumpTableBranches->count(Addr)))
      continue;
    rewriteAsIndirectTailCall(Rec);
    Changed = true;
  }
  if (Changed)
    rebuildBlocks(Func);
}

//===----------------------------------------------------------------------===//
// classifyInsn — set control-flow flags from decoded ops
//===----------------------------------------------------------------------===//

void CFGBuilder::classifyInsn(InsnRecord &Rec) {
  const std::optional<uint64_t> ReturnImmediate = Rec.Immediate;
  std::optional<uint64_t> BranchImmediate;
  std::optional<uint64_t> CallImmediate;
  for (auto &Op : Rec.Ops) {
    switch (Op.Opcode) {
    case NdOp::BRANCH:
      Rec.IsBranch = true;
      if (Op.NumInputs >= 1 && Op.Inputs[0].isConst()) {
        Rec.BranchTarget = Op.Inputs[0].Offset;
        BranchImmediate = Op.Inputs[0].Offset;
      }
      break;
    case NdOp::COND_BR:
      Rec.IsBranch = true;
      Rec.IsCond = true;
      if (Op.NumInputs >= 1 && Op.Inputs[0].isConst()) {
        Rec.BranchTarget = Op.Inputs[0].Offset;
        BranchImmediate = Op.Inputs[0].Offset;
      }
      break;
    case NdOp::INDIR_BR:
      Rec.IsBranch = true;
      Rec.IsIndirect = true;
      break;
    case NdOp::CALL:
      Rec.IsCall = true;
      if (Op.NumInputs >= 1 && Op.Inputs[0].isConst()) {
        CallTargets.insert(Op.Inputs[0].Offset);
        CallImmediate = Op.Inputs[0].Offset;
      }
      break;
    case NdOp::INDIR_CALL:
      Rec.IsCall = true;
      Rec.IsIndirect = true;
      break;
    case NdOp::RETURN:
      Rec.IsRet = true;
      break;
    default:
      break;
    }
  }

  Rec.IsInstructionGuard = hasInstructionLocalGuard(Rec);

  // Preserve the immediate encoded by the guest instruction, not a synthetic
  // conditional guard the ARM lifter emitted around a register call/return.
  if (Rec.IsCall)
    Rec.Immediate = Rec.IsIndirect ? std::nullopt : CallImmediate;
  else if (Rec.IsRet)
    Rec.Immediate = ReturnImmediate;
  else if (Rec.IsBranch)
    Rec.Immediate = Rec.IsIndirect ? std::nullopt : BranchImmediate;
  else
    Rec.Immediate.reset();
}

bool CFGBuilder::resolveConstantIndirectBranch(const BinaryImage &Img,
                                               uint32_t InsnId,
                                               InsnRecord &Rec) {
  // This exact A32 veneer reads the adjacent immutable literal into PC.
  // Decode its file-backed target as a direct edge so a patched entry can be
  // analyzed again, including a Thumb target selected by pointer bit 0.
  if (Img.Arch == Arch::ARM && Rec.Mode == InstructionMode::ARM &&
      InsnId == ARM_INS_LDR && Rec.IsBranch && Rec.IsIndirect && !Rec.IsCall &&
      !Rec.IsCond) {
    const auto Bytes =
        readImmutableCodeBytes(Img, Rec.Addr, arm::kLdrPCTrampLen);
    if (Bytes && readLE<uint32_t>(Bytes->data()) == arm::kLdrPC) {
      const uint32_t TaggedTarget =
          readLE<uint32_t>(Bytes->data() + arm::kInsnSize);
      const va_t Target = clearThumbBit(TaggedTarget);
      const InstructionMode TargetMode =
          (TaggedTarget & 1u) ? InstructionMode::Thumb : InstructionMode::ARM;
      if (Img.hasExecutableCodeOwnerAt(Target) &&
          Img.instructionModeAt(Target, TargetMode) == TargetMode) {
        for (LowOp &Op : Rec.Ops) {
          if (Op.Opcode != NdOp::INDIR_BR || Op.NumInputs < 1)
            continue;
          Op.Opcode = NdOp::BRANCH;
          Op.Inputs[0] = NdVar::cst(Target, 4);
          Rec.IsIndirect = false;
          Rec.BranchTarget = Target;
          Rec.TargetMode = TargetMode == InstructionMode::Thumb
                               ? LowInstructionTargetMode::Thumb
                               : LowInstructionTargetMode::ARM;
          return true;
        }
      }
    }
  }

  // Only AArch64 RET with an explicit non-LR register is lifted as INDIR_BR
  // for this purpose.  Folding an ordinary BR/jump here can sample a dynamic
  // table load at its default index and collapse a real jump table to one arm.
  if (Img.Arch != Arch::AArch64 || InsnId != AARCH64_INS_RET || !Rec.IsBranch ||
      !Rec.IsIndirect || Rec.IsCall || Rec.IsCond)
    return false;

  for (LowOp &Op : Rec.Ops) {
    if (Op.Opcode != NdOp::INDIR_BR || Op.NumInputs < 1 ||
        !Op.Inputs[0].isReg())
      continue;

    const uint16_t TargetSize = Op.Inputs[0].Size;
    auto Target = foldRegConstant(Img, Rec, Op.Inputs[0].Offset);
    if (!Target)
      return false;
    if (!Img.hasExecutableCodeOwnerAt(*Target) ||
        (*Target % getInsnAlignment()) != 0)
      return false;

    Op.Opcode = NdOp::BRANCH;
    Op.Inputs[0] = NdVar::cst(*Target, TargetSize);
    Rec.IsIndirect = false;
    Rec.BranchTarget = *Target;
    return true;
  }
  return false;
}

LowInstructionBoundary
CFGBuilder::makeInstructionBoundary(const InsnRecord &Rec,
                                    uint64_t FirstOp) const {
  LowInstructionBoundary Boundary;
  Boundary.Address = Rec.Addr;
  Boundary.Size = Rec.Size;
  Boundary.FirstOp = FirstOp;
  Boundary.OpCount = static_cast<uint64_t>(Rec.Ops.size());
  Boundary.Mode = Rec.Mode;
  Boundary.Immediate = Rec.Immediate;
  Boundary.TargetMode = Rec.TargetMode;

  auto AddFlag = [&](bool Set, LowInstructionControlFlag Flag) {
    if (Set)
      Boundary.ControlFlags |= Flag;
  };

  if (Rec.IsOpaqueTerminator) {
    Boundary.Control = LowInstructionControl::Terminator;
    Boundary.ControlFlags = LowInstructionControlFlag::Terminator;
    AddFlag(Rec.IsResumableTerminator, LowInstructionControlFlag::Resumable);
    return Boundary;
  }

  AddFlag(Rec.IsBranch, LowInstructionControlFlag::Branch);
  AddFlag(Rec.IsCond, LowInstructionControlFlag::Conditional);
  AddFlag(Rec.IsCall, LowInstructionControlFlag::Call);
  AddFlag(Rec.IsRet, LowInstructionControlFlag::Return);
  AddFlag(Rec.IsIndirect, LowInstructionControlFlag::Indirect);
  AddFlag(Rec.IsNoReturnCall, LowInstructionControlFlag::NoReturn);
  AddFlag(Rec.IsInstructionGuard, LowInstructionControlFlag::InstructionGuard);

  if (Rec.IsCall && Rec.IsRet)
    Boundary.Control = LowInstructionControl::TailCall;
  else if (Rec.IsBranch && Rec.IsCond && Rec.IsRet)
    Boundary.Control = LowInstructionControl::ConditionalReturn;
  else if (Rec.IsBranch && Rec.IsCond && Rec.IsCall)
    Boundary.Control = LowInstructionControl::ConditionalCall;
  else if (Rec.IsBranch)
    Boundary.Control = LowInstructionControl::Branch;
  else if (Rec.IsCall)
    Boundary.Control = LowInstructionControl::Call;
  else if (Rec.IsRet)
    Boundary.Control = LowInstructionControl::Return;
  return Boundary;
}

//===----------------------------------------------------------------------===//
// isTailCallTarget / rewriteAsTailCall — model `jmp other_func` as call + ret
//===----------------------------------------------------------------------===//

bool CFGBuilder::hasFunctionSymbolAtIndexed(const BinaryImage &Img, va_t Addr) {
  if (FunctionSymbolIndexImage != &Img ||
      FunctionSymbolIndexData != Img.Symbols.data() ||
      FunctionSymbolIndexCount != Img.Symbols.size()) {
    FunctionSymbolIndex.clear();
    for (const Symbol &Sym : Img.Symbols)
      if (Sym.IsFunc)
        FunctionSymbolIndex.push_back(
            normalizeCodeAddress(Sym.Addr, Img.Arch, Img.Mode));
    std::sort(FunctionSymbolIndex.begin(), FunctionSymbolIndex.end());
    FunctionSymbolIndexImage = &Img;
    FunctionSymbolIndexData = Img.Symbols.data();
    FunctionSymbolIndexCount = Img.Symbols.size();
  }
  return std::binary_search(FunctionSymbolIndex.begin(),
                            FunctionSymbolIndex.end(),
                            normalizeCodeAddress(Addr, Img.Arch, Img.Mode));
}

bool CFGBuilder::isTailCallTarget(va_t Target) const {
  if (Target == InvalidVA || Target == CurrentFuncEntry)
    return false;
  // A selected-function run may omit other function symbols from its work
  // set. An explicit transfer to such a symbol still names a tail callee.
  // Symbol identity alone must not stop ordinary fallthrough exploration:
  // x86 call-next/POP retains its architectural push even at a named address.
  const bool FunctionEntry = isKnownFunctionEntry(Target) ||
                             (CurrentImg && CurrentImg->hasFunctionSymbolAt(
                                                Target, ExecutableCodeOwners));
  if (FunctionEntry && !isCurrentExceptionalEntry(Target) &&
      !isCurrentOwnedFragment(Target))
    return true;
  // A direct branch landing on a registered import veneer is a tail call to
  // that external function.  Without this the veneer is followed and inlined
  // into the caller as an untyped indirect call, dropping import identity and
  // ABI.  BinaryImage resolves both the format-native IAT address and exact
  // executable stubs, so this also covers COFF thunks whose IATAddr stays a
  // data slot.
  if (CurrentImg && CurrentImg->findImportAt(Target))
    return true;
  return false;
}

bool CFGBuilder::isNoReturnCall(const InsnRecord &Rec) const {
  if (!CurrentImg)
    return false;
  va_t Target = InvalidVA;
  for (const auto &Op : Rec.Ops)
    if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
        Op.Inputs[0].isConst()) {
      Target = Op.Inputs[0].Offset;
      break;
    }
  if (Target == InvalidVA)
    return false;
  if (libc::isNoReturnTarget(*CurrentImg, Target, NoReturnTargets))
    return true;
  // An internal callee needs a proof, which lifts it; padding after the call
  // is where a compiler that knew the callee never returns left its trace.
  return NoReturnCallees && CurrentImg->hasExecutableCodeOwnerAt(Target) &&
         callIsFollowedByPadding(Rec) &&
         NoReturnCallees->neverReturns(Target, NoReturnCalleeDepth);
}

bool CFGBuilder::callIsFollowedByPadding(const InsnRecord &Rec) const {
  const va_t Next = Rec.Addr + Rec.Size;
  if (isKnownFunctionEntry(Next))
    return true;
  if (CurrentImg->Arch != Arch::X86 && CurrentImg->Arch != Arch::X64)
    return false;
  const Segment *Seg = CurrentImg->getSegmentFor(Next);
  return Seg && Next >= Seg->VA && Next - Seg->VA < Seg->Data.size() &&
         Seg->Data[static_cast<size_t>(Next - Seg->VA)] == x86::kInt3;
}

void CFGBuilder::restoreAdjacentNoReturnCall(InsnRecord &Rec,
                                             va_t DecodedCallTarget) {
  if (!CurrentImg ||
      (CurrentImg->Arch != Arch::X86 && CurrentImg->Arch != Arch::X64) ||
      DecodedCallTarget == InvalidVA ||
      DecodedCallTarget != Rec.Addr + Rec.Size || Rec.Ops.size() != 2)
    return;
  const auto &TRI = getTargetRegInfo(CurrentImg->Arch);
  const auto Stack = NdVar::reg(TRI.StackPointer, TRI.PointerSize);
  const auto &Adjust = Rec.Ops[0];
  const auto &Push = Rec.Ops[1];
  if (Adjust.Opcode != NdOp::INT_SUB || Adjust.NumInputs != 2 ||
      Adjust.Output != Stack || Adjust.Inputs[0] != Stack ||
      !Adjust.Inputs[1].isConst() ||
      Adjust.Inputs[1].Offset != TRI.PointerSize ||
      Push.Opcode != NdOp::STORE || Push.NumInputs != 2 ||
      Push.Inputs[0] != Stack || !Push.Inputs[1].isConst() ||
      Push.Inputs[1].Size != TRI.PointerSize ||
      Push.Inputs[1].Offset != DecodedCallTarget)
    return;
  InsnRecord Candidate = Rec;
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = Rec.Addr;
  Call.Seq = Adjust.Seq;
  Call.Output = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
  Call.addInput(Push.Inputs[1]);
  Candidate.Ops = {Call};
  // The established no-return target index owns this decision. Ordinary
  // call-next/POP sequences keep their physical push and architectural PC.
  if (isNoReturnCall(Candidate))
    Rec.Ops = std::move(Candidate.Ops);
}

void CFGBuilder::rewriteAsTailCall(InsnRecord &Rec) {
  if (!CurrentImg)
    return;
  va_t Target = Rec.BranchTarget;

  // The instruction's only effect is the control transfer, so drop every op it
  // lifted to (e.g. x86 emits a lone BRANCH; ARM also emits a `COPY PC, target`
  // pipeline model whose leftover constant would otherwise pollute
  // call-argument recovery) and replace them with CALL(retReg, target) +
  // RETURN(retReg), mirroring a real `call target; ret`.  Downstream ABI
  // recovery then recovers the call arguments set up before the branch.
  Rec.Ops = directTailCallOperations(CurrentImg->Arch, Rec.Addr, Target);

  Rec.IsBranch = false;
  Rec.IsCond = false;
  Rec.IsIndirect = false;
  Rec.IsCall = true;
  Rec.IsRet = true;
  Rec.BranchTarget = InvalidVA;
  CallTargets.insert(Target);
}

void CFGBuilder::rewriteOwnInteriorCall(const BinaryImage &Img, InsnRecord &Rec,
                                        va_t Next) {
  if (Img.Arch != Arch::X64 || Img.Format != BinaryFormat::COFF)
    return;
  // Compiler output reaches its own code with jumps; only hand-written code
  // calls a label inside itself, and the return address that pushes is a
  // value like any other.  Whether a return later pops it is for
  // classifyOwnInteriorCalls to prove.
  const va_t Target = *Rec.Immediate;
  const ExceptionFunction *Own =
      Img.ExceptionMetadata.findFunction(CurrentFuncEntry);
  if (!Own || Own->Kind != RuntimeFunctionKind::Primary ||
      Own->CodeRange.Begin != CurrentFuncEntry ||
      !Own->CodeRange.contains(Rec.Addr) || !Own->CodeRange.contains(Target) ||
      Target == Own->CodeRange.Begin ||
      (KnownFuncEntries && KnownFuncEntries->count(Target)) ||
      KeptOwnInteriorCallTargets.count(Target))
    return;
  const uint16_t PtrSize = 8;
  const NdVar Rsp = NdVar::reg(x86reg::RSP, PtrSize);
  LowOp Sub;
  Sub.Opcode = NdOp::INT_SUB;
  Sub.Output = Rsp;
  Sub.addInput(Rsp);
  Sub.addInput(NdVar::scalar(PtrSize, PtrSize));
  Sub.Addr = Rec.Addr;
  Sub.Seq = 0;
  LowOp Store;
  Store.Opcode = NdOp::STORE;
  Store.addInput(Rsp);
  Store.addInput(NdVar::cst(Next, PtrSize));
  Store.Addr = Rec.Addr;
  Store.Seq = 1;
  LowOp Jump;
  Jump.Opcode = NdOp::BRANCH;
  Jump.addInput(NdVar::cst(Target, PtrSize));
  Jump.Addr = Rec.Addr;
  Jump.Seq = 2;
  Rec.Ops = {Sub, Store, Jump};
  Rec.IsCall = false;
  Rec.IsBranch = true;
  Rec.BranchTarget = Target;
  CallTargets.erase(Target);
  OwnInteriorCallSites[Target].insert(Rec.Addr);
}

void CFGBuilder::rewriteAsIndirectTailCall(InsnRecord &Rec) {
  if (!CurrentImg)
    return;

  // Capture the indirect branch target (the function-pointer register/temp)
  // and its instruction-local definition slice before replacing the control
  // transfer.  x86 memory-indirect JMP materializes the effective address and
  // LOAD in the same instruction, and LiftState temporary ids are reused by
  // every later instruction.  Keeping only the naked temp after clearing the
  // record therefore reconnects the tail call to an unrelated later temp once
  // SSA is built (often a one-byte flag calculation).
  NdVar Target;
  size_t BranchIndex = Rec.Ops.size();
  for (size_t I = 0; I < Rec.Ops.size(); ++I)
    if (Rec.Ops[I].Opcode == NdOp::INDIR_BR && Rec.Ops[I].NumInputs >= 1) {
      Target = Rec.Ops[I].Inputs[0];
      BranchIndex = I;
      break;
    }
  if (BranchIndex == Rec.Ops.size())
    return;

  auto sameStorage = [](const NdVar &Left, const NdVar &Right) {
    return Left.Space == Right.Space && Left.Offset == Right.Offset &&
           Left.Size == Right.Size;
  };
  std::vector<NdVar> Needed{Target};
  std::vector<bool> Keep(BranchIndex, false);
  for (size_t I = BranchIndex; I-- > 0;) {
    const LowOp &Candidate = Rec.Ops[I];
    if (Candidate.Output.Size == 0)
      continue;
    const bool DefinesNeeded =
        std::any_of(Needed.begin(), Needed.end(), [&](const NdVar &Value) {
          return sameStorage(Candidate.Output, Value);
        });
    if (!DefinesNeeded)
      continue;
    Keep[I] = true;
    for (uint8_t Input = 0; Input < Candidate.NumInputs; ++Input)
      if (!Candidate.Inputs[Input].isConst())
        Needed.push_back(Candidate.Inputs[Input]);
  }

  const auto &TRI = getTargetRegInfo(CurrentImg->Arch);
  NdVar RetReg = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
  va_t At = Rec.Addr;

  // Replace `bx reg` (+ any unrelated ARM `COPY PC, next` pipeline model) with
  // an indirect call followed by a return.  Retain only the backward slice
  // that computes an instruction-local target, so a memory-indirect x86 tail
  // call keeps its address arithmetic and LOAD without leaking unrelated
  // architectural bookkeeping into call-argument recovery.
  std::vector<LowOp> Rewritten;
  Rewritten.reserve(BranchIndex + 2);
  for (size_t I = 0; I < BranchIndex; ++I)
    if (Keep[I])
      Rewritten.push_back(Rec.Ops[I]);

  LowOp Call;
  Call.Opcode = NdOp::INDIR_CALL;
  Call.Output = RetReg;
  Call.addInput(Target);
  Call.Addr = At;
  Rewritten.push_back(Call);

  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.addInput(RetReg);
  Ret.Addr = At;
  Rewritten.push_back(Ret);
  Rec.Ops = std::move(Rewritten);

  Rec.IsBranch = false;
  Rec.IsCond = false;
  Rec.IsIndirect = true;
  Rec.IsCall = true;
  Rec.IsRet = true;
  Rec.BranchTarget = InvalidVA;
}

} // namespace neverd
