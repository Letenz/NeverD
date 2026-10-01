//===- LLVMInterpreterModelScalar.cpp - Scalar LLVM model ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LLVMInterpreterModelInternal.h"

namespace neverd::analysis::llvm_model {
void Builder::requireEqual(LowBlock &Out, NdVar A, NdVar B) {
  auto Bad = local(1);
  emit(Out, op(NdOp::INT_NOTEQUAL, Bad, {A, B}));
  emit(Out, op(NdOp::INT_OR, rvar(DefinednessOffset, 1),
               {rvar(DefinednessOffset, 1), Bad}));
}
void Builder::requireRange(LowBlock &Out, NdVar Value,
                           const llvm::ConstantRange &Range) {
  if (Range.isFullSet())
    return;
  if (Range.isEmptySet()) {
    requireEqual(Out, num(0, 1), num(1, 1));
    return;
  }
  auto Lo = local(1), Hi = local(1), Bad = local(1);
  emit(Out, op(NdOp::INT_LESS, Lo,
               {Value, num(Range.getLower().getZExtValue(), Value.Size)}));
  emit(Out, op(NdOp::INT_LESSEQUAL, Hi,
               {num(Range.getUpper().getZExtValue(), Value.Size), Value}));
  emit(Out,
       op(Range.getLower().uge(Range.getUpper()) ? NdOp::INT_AND : NdOp::INT_OR,
          Bad, {Lo, Hi}));
  requireEqual(Out, Bad, num(0, 1));
}
bool Builder::emitScalar(LowBlock &Out, const llvm::Instruction &I) {
  if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(&I)) {
    if (!Cast->getSrcTy()->isIntegerTy() || !Cast->getDestTy()->isIntegerTy() ||
        Cast->hasMetadataOtherThanDebugLoc())
      fail("unsupported integer cast contract");
    auto In = value(Cast->getOperand(0)), Dest = value(&I);
    if (auto *Trunc = llvm::dyn_cast<llvm::TruncInst>(Cast)) {

      if (Cast->getDestTy()->isIntegerTy(1))
        emit(Out, op(NdOp::INT_AND, Dest, {In, num(1, In.Size)}));
      else
        emit(Out, op(NdOp::COPY, Dest, {In}));
      if (Trunc->hasNoUnsignedWrap()) {
        auto Back = local(In.Size);
        emit(Out, op(NdOp::INT_ZEXT, Back, {Dest}));
        requireEqual(Out, Back, In);
      }
      if (Trunc->hasNoSignedWrap()) {
        auto Back = local(In.Size);
        if (Cast->getDestTy()->isIntegerTy(1)) {
          auto Extended = local(In.Size);
          emit(Out, op(NdOp::INT_ZEXT, Extended, {Dest}));
          emit(Out, op(NdOp::INT_SUB, Back, {num(0, In.Size), Extended}));
        } else
          emit(Out, op(NdOp::INT_SEXT, Back, {Dest}));
        requireEqual(Out, Back, In);
      }
    } else if (Cast->getOpcode() == llvm::Instruction::ZExt) {
      if (auto *NonNeg = llvm::dyn_cast<llvm::PossiblyNonNegInst>(Cast);
          NonNeg && NonNeg->hasNonNeg()) {
        auto Bad = local(1);
        if (Cast->getSrcTy()->isIntegerTy(1))
          emit(Out, op(NdOp::COPY, Bad, {In}));
        else
          emit(Out, op(NdOp::INT_SLESS, Bad, {In, num(0, In.Size)}));
        requireEqual(Out, Bad, num(0, 1));
      }
      emit(Out, op(NdOp::INT_ZEXT, Dest, {In}));
    } else if (Cast->getOpcode() == llvm::Instruction::SExt) {
      if (Cast->getSrcTy()->isIntegerTy(1)) {
        auto Extended = local(Dest.Size);
        emit(Out, op(NdOp::INT_ZEXT, Extended, {In}));
        emit(Out, op(NdOp::INT_SUB, Dest, {num(0, Dest.Size), Extended}));
      } else
        emit(Out, op(NdOp::INT_SEXT, Dest, {In}));
    } else
      fail("unsupported integer cast");
    return true;
  }
  if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(&I)) {
    if (!Select->getCondition()->getType()->isIntegerTy(1) ||
        !Select->getType()->isIntegerTy() ||
        Select->hasMetadataOtherThanDebugLoc())
      fail("unsupported select contract");
    emit(Out, op(NdOp::SELECT, value(&I),
                 {value(Select->getCondition()), value(Select->getTrueValue()),
                  value(Select->getFalseValue())}));
    return true;
  }
  if (auto *Extract = llvm::dyn_cast<llvm::ExtractValueInst>(&I)) {
    auto It = Aggregates.find(Extract->getAggregateOperand());
    if (It == Aggregates.end() || Extract->getNumIndices() != 1 ||
        *Extract->idx_begin() > 1 || Extract->hasMetadataOtherThanDebugLoc())
      fail("unsupported aggregate extraction");
    emit(Out,
         op(NdOp::COPY, value(&I),
            {*Extract->idx_begin() ? It->second.second : It->second.first}));
    return true;
  }
  if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&I)) {
    auto *Callee = Call->getCalledFunction();
    auto ID =
        Callee ? Callee->getIntrinsicID() : llvm::Intrinsic::not_intrinsic;
    bool Pop = ID == llvm::Intrinsic::ctpop,
         Add = ID == llvm::Intrinsic::sadd_with_overflow,
         Sub = ID == llvm::Intrinsic::ssub_with_overflow;
    if (!Callee || !Callee->isDeclaration() || (!Pop && !Add && !Sub) ||
        Call->arg_size() != (Pop ? 1U : 2U) ||
        !Call->getArgOperand(0)->getType()->isIntegerTy() ||
        Call->getArgOperand(0)->getType()->isIntegerTy(1) ||
        Call->getCallingConv() != llvm::CallingConv::C ||
        Call->isMustTailCall() || Call->hasOperandBundles() ||
        Call->hasMetadataOtherThanDebugLoc() ||
        Call->getAttributes().getFnAttrs().hasAttributes() ||
        Callee->getAttributes() !=
            llvm::Intrinsic::getAttributes(F.getContext(), ID,
                                           Callee->getFunctionType()))
      fail("unsupported intrinsic or call contract");
    for (unsigned N = 0; N < Call->arg_size(); ++N)
      if (Call->getAttributes().getParamAttrs(N).hasAttributes())
        fail("unsupported intrinsic argument contract");
    for (auto A : Call->getAttributes().getRetAttrs())
      if (!Pop || A.isStringAttribute() ||
          A.getKindAsEnum() != llvm::Attribute::Range)
        fail("unsupported intrinsic return contract");
    if (Pop) {
      if (Call->getType() != Call->getArgOperand(0)->getType())
        fail("invalid ctpop type");
      emit(Out, op(NdOp::POPCOUNT, value(&I), {value(Call->getArgOperand(0))}));
      if (auto Range = Call->getRange())
        requireRange(Out, value(&I), *Range);
    } else {
      auto [Sum, Overflow] = Aggregates.at(Call);
      auto A = value(Call->getArgOperand(0)), B = value(Call->getArgOperand(1));
      emit(Out, op(Add ? NdOp::INT_ADD : NdOp::INT_SUB, Sum, {A, B}));
      emit(Out, op(Add ? NdOp::INT_SOVF : NdOp::INT_SBOR, Overflow, {A, B}));
    }
    return true;
  }
  if (auto *G = llvm::dyn_cast<llvm::GetElementPtrInst>(&I))
    fail("guest GEP obligations unsupported; use explicit integer address "
         "arithmetic");
  if (auto *Binary = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
    NdOp K;
    switch (I.getOpcode()) {
    case llvm::Instruction::Add:
      K = NdOp::INT_ADD;
      break;
    case llvm::Instruction::Sub:
      K = NdOp::INT_SUB;
      break;
    case llvm::Instruction::Mul:
      K = NdOp::INT_MULT;
      break;
    case llvm::Instruction::And:
      K = NdOp::INT_AND;
      break;
    case llvm::Instruction::Or:
      K = NdOp::INT_OR;
      break;
    case llvm::Instruction::Xor:
      K = NdOp::INT_XOR;
      break;
    case llvm::Instruction::Shl:
      K = NdOp::INT_LEFT;
      break;
    case llvm::Instruction::LShr:
      K = NdOp::INT_RIGHT;
      break;
    case llvm::Instruction::AShr:
      K = NdOp::INT_ASHR;
      break;
    default:
      fail("binary operation unsupported");
    }
    if (I.getType()->isIntegerTy(1) && K != NdOp::INT_AND &&
        K != NdOp::INT_OR && K != NdOp::INT_XOR)
      fail("one-bit arithmetic unsupported");
    if (I.isShift()) {
      const unsigned Bits = I.getType()->getIntegerBitWidth();
      if (auto *Constant = llvm::dyn_cast<llvm::ConstantInt>(I.getOperand(1))) {
        if (Constant->getZExtValue() >= Bits)
          fail("unproved shift definedness");
      } else {
        // LLVM treats the whole unsigned count as poison-producing when it
        // reaches the operand width. LowIR's total shift or an ISA count mask
        // cannot discharge that source obligation.
        auto Count = value(I.getOperand(1)), InRange = local(1);
        emit(Out, op(NdOp::INT_LESS, InRange, {Count, num(Bits, Count.Size)}));
        requireEqual(Out, InRange, num(1, 1));
      }
    }
    emit(Out,
         op(K, value(&I), {value(I.getOperand(0)), value(I.getOperand(1))}));
    auto A = value(I.getOperand(0)), B = value(I.getOperand(1)), R = value(&I);
    if (auto *O = llvm::dyn_cast<llvm::OverflowingBinaryOperator>(&I);
        O && (O->hasNoSignedWrap() || O->hasNoUnsignedWrap())) {
      if (K == NdOp::INT_ADD || K == NdOp::INT_SUB) {
        if (O->hasNoUnsignedWrap()) {
          auto Bad = local(1);
          emit(Out, op(K == NdOp::INT_ADD ? NdOp::INT_CARRY : NdOp::INT_LESS,
                       Bad, {A, B}));
          requireEqual(Out, Bad, num(0, 1));
        }
        if (O->hasNoSignedWrap()) {
          auto Bad = local(1);
          emit(Out, op(K == NdOp::INT_ADD ? NdOp::INT_SOVF : NdOp::INT_SBOR,
                       Bad, {A, B}));
          requireEqual(Out, Bad, num(0, 1));
        }
      } else if (K == NdOp::INT_LEFT) {
        if (O->hasNoUnsignedWrap()) {
          auto Back = local(A.Size);
          emit(Out, op(NdOp::INT_RIGHT, Back, {R, B}));
          requireEqual(Out, Back, A);
        }
        if (O->hasNoSignedWrap()) {
          auto Back = local(A.Size);
          emit(Out, op(NdOp::INT_ASHR, Back, {R, B}));
          requireEqual(Out, Back, A);
        }
      } else
        fail("unsupported guarded no-wrap operation");
    }
    if (auto *O = llvm::dyn_cast<llvm::PossiblyDisjointInst>(&I);
        O && O->isDisjoint()) {
      auto Shared = local(A.Size);
      emit(Out, op(NdOp::INT_AND, Shared, {A, B}));
      requireEqual(Out, Shared, num(0, A.Size));
    }
    if (auto *O = llvm::dyn_cast<llvm::PossiblyExactOperator>(&I);
        O && O->isExact()) {
      if (K != NdOp::INT_RIGHT && K != NdOp::INT_ASHR)
        fail("unsupported guarded exact operation");
      auto Back = local(A.Size);
      emit(Out, op(NdOp::INT_LEFT, Back, {R, B}));
      requireEqual(Out, Back, A);
    }

    return true;
  }
  if (auto *Cmp = llvm::dyn_cast<llvm::ICmpInst>(&I)) {
    if (!Cmp->getOperand(0)->getType()->isIntegerTy())
      fail("noninteger comparison unsupported");
    if (Cmp->hasSameSign()) {
      auto A = value(I.getOperand(0)), B = value(I.getOperand(1)),
           SA = local(1), SB = local(1);
      if (Cmp->getOperand(0)->getType()->isIntegerTy(1)) {
        requireEqual(Out, A, B);
      } else {
        emit(Out, op(NdOp::INT_SLESS, SA, {A, num(0, A.Size)}));
        emit(Out, op(NdOp::INT_SLESS, SB, {B, num(0, B.Size)}));
        requireEqual(Out, SA, SB);
      }
    }
    if (Cmp->getOperand(0)->getType()->isIntegerTy(1) && Cmp->isSigned())
      fail("signed one-bit comparison unsupported");
    NdOp K;
    auto A = value(I.getOperand(0)), B = value(I.getOperand(1));
    switch (Cmp->getPredicate()) {
    case llvm::CmpInst::ICMP_EQ:
      K = NdOp::INT_EQUAL;
      break;
    case llvm::CmpInst::ICMP_NE:
      K = NdOp::INT_NOTEQUAL;
      break;
    case llvm::CmpInst::ICMP_ULT:
      K = NdOp::INT_LESS;
      break;
    case llvm::CmpInst::ICMP_ULE:
      K = NdOp::INT_LESSEQUAL;
      break;
    case llvm::CmpInst::ICMP_SLT:
      K = NdOp::INT_SLESS;
      break;
    case llvm::CmpInst::ICMP_SLE:
      K = NdOp::INT_SLESSEQUAL;
      break;
    case llvm::CmpInst::ICMP_UGT:
      K = NdOp::INT_LESS;
      std::swap(A, B);
      break;
    case llvm::CmpInst::ICMP_UGE:
      K = NdOp::INT_LESSEQUAL;
      std::swap(A, B);
      break;
    case llvm::CmpInst::ICMP_SGT:
      K = NdOp::INT_SLESS;
      std::swap(A, B);
      break;
    case llvm::CmpInst::ICMP_SGE:
      K = NdOp::INT_SLESSEQUAL;
      std::swap(A, B);
      break;
    default:
      fail("comparison unsupported");
    }
    emit(Out, op(K, value(&I), {A, B}));
    return true;
  }
  return false;
}

} // namespace neverd::analysis::llvm_model
