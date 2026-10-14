//===- RegistrationCall.cpp - Bind original PE32 call contracts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/X86RegistrationCall.h"
#include "neverd/lift/X86Regs.h"

namespace neverd {

llvm::ArrayRef<uint64_t> RegistrationCallABI::integerArgumentRegisters() const {
  static constexpr uint64_t ThisRegister[] = {x86reg::RCX};
  return BorrowsECX ? llvm::ArrayRef<uint64_t>(ThisRegister)
                    : llvm::ArrayRef<uint64_t>{};
}

std::optional<uint32_t> registrationCallStackPop(const LowFunc &Func,
                                                 const LowBlock &Block,
                                                 const LowOp &Call) {
  if (!Func.ExceptionMetadata || !Func.RegistrationStates ||
      !Func.RegistrationStates->Complete ||
      !Func.RegistrationStates->CallFrameEffectsComplete ||
      Call.Opcode != NdOp::CALL || Call.NumInputs != 1 ||
      !Call.Inputs[0].isConst() || Call.Inputs[0].Size != 4 || Call.Seq < 0)
    return std::nullopt;
  const auto &States = *Func.RegistrationStates;
  const auto *Effect = States.callFrameEffect(Call.Addr, Call.Seq);
  if (!Effect || Effect->Target != Call.Inputs[0].Offset ||
      Effect->CalleeIndex >= States.CalleeContracts.size() ||
      Effect->DoesNotReturn || Call.Addr < Block.StartAddr ||
      Effect->EndAddress > Block.EndAddr || Effect->EndAddress <= Call.Addr ||
      !Func.ExceptionMetadata->ownsCode({Call.Addr, Effect->EndAddress}))
    return std::nullopt;
  const auto &Callee = States.CalleeContracts[Effect->CalleeIndex];
  if ((Callee.CalleeKind != RegistrationCalleeFrameContract::Kind::Leaf &&
       !Callee.isInitializer()) ||
      Callee.Target != Effect->Target ||
      Callee.StackPopBytes != Effect->StackPopBytes ||
      Callee.StackPopBytes > UINT16_MAX || Callee.StackPopBytes % 4)
    return std::nullopt;
  return Callee.StackPopBytes;
}

std::optional<RegistrationCallABI> registrationCallABI(const MedFunc &Func,
                                                       const MedBlock &Block,
                                                       const MedOp &Call) {
  if (!Func.ExceptionMetadata || !Func.RegistrationStates || Func.SkippedSSA ||
      Call.Opcode != NdOp::CALL || Call.Dead || Call.NumInputs != 1 ||
      !Call.Inputs[0].isConst() || Call.Inputs[0].Size != 4 ||
      Call.Inputs[0].ConstVal > UINT32_MAX || Call.OriginSeq < 0 ||
      Call.ExactArguments || Call.SourceCallHint ||
      Func.Blocks.size() > limits::kMaxRegistrationEHRecords)
    return std::nullopt;
  const auto &EH = *Func.ExceptionMetadata;
  const auto &State = *Func.RegistrationStates;
  if (!EH.Registration || EH.Encoding != ExceptionEncoding::X86CxxFuncInfo ||
      EH.ParseStatus != ExceptionParseStatus::Complete ||
      Func.Entry != EH.CodeRange.Begin || !State.Complete ||
      !State.CallFrameEffectsComplete ||
      State.CallFrameEffects.size() > limits::kMaxRegistrationEHRecords ||
      State.CalleeContracts.size() > limits::kMaxRegistrationEHRecords)
    return std::nullopt;
  const RegistrationCallFrameEffect *Effect = nullptr;
  for (const auto &Candidate : State.CallFrameEffects)
    if (Candidate.Address == Call.Addr && Candidate.OpSeq == Call.OriginSeq) {
      if (Effect)
        return std::nullopt;
      Effect = &Candidate;
    }
  if (!Effect || Effect->Target != Call.Inputs[0].ConstVal ||
      Effect->CalleeIndex >= State.CalleeContracts.size() ||
      Effect->DoesNotReturn != Call.DoesNotReturn ||
      Call.Addr < Block.StartAddr || Effect->EndAddress <= Call.Addr ||
      Effect->EndAddress > Block.EndAddr ||
      !EH.ownsCode({Call.Addr, Effect->EndAddress}))
    return std::nullopt;
  const auto &Callee = State.CalleeContracts[Effect->CalleeIndex];
  if (Callee.Target != Effect->Target ||
      Callee.StackPopBytes != Effect->StackPopBytes ||
      Callee.StackPopBytes > UINT16_MAX || Callee.StackPopBytes % 4 ||
      Callee.StackPopBytes / 4 >= limits::kMaxCallArgs ||
      (Callee.isThrow() && Callee.StackPopBytes) ||
      Callee.isRuntimeThrow() != Effect->RuntimeThrow.has_value() ||
      Callee.DoesNotReturn != Effect->DoesNotReturn)
    return std::nullopt;
  const bool Borrow = !Callee.ECXReads.empty() || !Callee.ECXWrites.empty();
  if ((Callee.StackPopBytes && !Borrow) ||
      (Effect->ECXFrameOffset && Effect->ECXRuntimeObject) ||
      (Effect->ECXRuntimeObject &&
       Callee.CalleeKind != RegistrationCalleeFrameContract::Kind::Leaf) ||
      Borrow != (Effect->ECXFrameOffset.has_value() ||
                 Effect->ECXRuntimeObject.has_value()) ||
      (Callee.isThrow() ? !Callee.DoesNotReturn || Borrow
                        : (Callee.CalleeKind !=
                               RegistrationCalleeFrameContract::Kind::Leaf &&
                           !Callee.isInitializer()) ||
                              Callee.DoesNotReturn))
    return std::nullopt;

  size_t Work = 0;
  bool Found = false;
  for (const auto &CurrentBlock : Func.Blocks)
    for (const auto &Current : CurrentBlock.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      if (Current.Addr != Call.Addr || Current.OriginSeq != Call.OriginSeq)
        continue;
      if (Found || &Current != &Call || &CurrentBlock != &Block)
        return std::nullopt;
      Found = true;
    }
  return Found ? std::optional(RegistrationCallABI{
                     Borrow, Callee.isRuntimeThrow(), Callee.isInitializer(),
                     Callee.StackPopBytes / 4})
               : std::nullopt;
}

} // namespace neverd
