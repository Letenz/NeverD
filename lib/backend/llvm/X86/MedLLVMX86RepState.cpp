//===- MedLLVMX86RepState.cpp - Native REP completion --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native intrinsics and the older-SDK assembly path share one full-register
/// result contract. Address-size arithmetic never substitutes for completion.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86/X86RepStateAsm.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/ErrorHandling.h"

#include <vector>

namespace neverd {

llvm::Value *MedLLVMEmitter::emitX86RepState(const MedOp &Op, Intrinsic Id,
                                             llvm::IRBuilder<> &B) {
  const auto Shape = x86RepMedShape(Op, TargetArch);
  if (!x86RepShapeIsValid(Id, Shape))
    llvm::report_fatal_error("invalid x86 REP completion contract");
  const auto P = *x86RepAsm(Id, TargetArch, *Shape.Descriptor);
  auto *Word = sizeToType(P.WordBytes);
  auto Coerce = [&](llvm::Value *V, llvm::Type *T) {
    return V->getType()->isPointerTy() ? B.CreatePtrToInt(V, T)
                                       : B.CreateZExtOrTrunc(V, T);
  };
  std::vector<llvm::Value *> Arguments;
  for (unsigned I = 1; I <= 3; ++I)
    Arguments.push_back(
        Coerce(getVar(Op.Inputs[I], B),
               I == 3 && P.Contract.Port ? sizeToType(2) : Word));
  auto *Status = Coerce(getVar(Op.Inputs[4], B), sizeToType(X86RepStatusBytes));
  auto *Direction = B.CreateAnd(B.CreateLShr(Status, X86RepDirectionShift), 1);
  auto *Flags = B.CreateAnd(Status, X86RepFlagsMask);
  const auto NativeID =
      llvm::Intrinsic::lookupIntrinsicID(x86RepNativeName(Id, TargetArch));
  auto *Function = B.GetInsertBlock()->getParent();
  if (P.Contract.Compare && TargetArch == Arch::X64) {
    auto Features =
        Function->getFnAttribute("target-features").getValueAsString().str();
    Function->addFnAttr("target-features",
                        Features.empty() ? "+sahf" : Features + ",+sahf");
  }
  llvm::CallInst *Call;
  if (NativeID) {
    Arguments.push_back(Coerce(Direction, sizeToType(1)));
    if (P.Contract.Compare)
      Arguments.push_back(Coerce(Flags, sizeToType(X86RepFlagBytes)));
    Arguments.push_back(B.getInt32(P.Control.ElementBytes));
    Arguments.push_back(B.getInt32(P.Control.AddressBytes * 8));
    if (P.Contract.Source)
      Arguments.push_back(B.getInt32(P.Control.Segment));
    if (P.Contract.Compare)
      Arguments.push_back(B.getInt32(P.Control.NotEqual));
    Call = B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                            Function->getParent(), NativeID),
                        Arguments);
  } else {
    Arguments.push_back(Direction);
    if (P.Contract.Compare)
      Arguments.push_back(Coerce(Flags, Word));
    std::vector<llvm::Type *> Types;
    for (const auto *V : Arguments)
      Types.push_back(V->getType());
    auto *Result = llvm::StructType::get(
        *Ctx, std::vector<llvm::Type *>(P.OutputConstraints.size(), Word));
    auto *Signature = llvm::FunctionType::get(Result, Types, false);
    auto *Asm = llvm::InlineAsm::get(Signature, P.Text, P.Constraints, true);
    Call = B.CreateCall(Asm, Arguments);
  }
  PendingIntrinsicCount = P.Contract.auxiliaryOutputs();
  for (unsigned I = 0; I < PendingIntrinsicCount; ++I) {
    auto *V = B.CreateExtractValue(Call, I + 1);
    if (I + 1 == P.Contract.WordResults)
      V = B.CreateAnd(Coerce(V, sizeToType(X86RepFlagBytes)), X86RepFlagsMask);
    PendingIntrinsicOutputs[I] = V;
  }
  return B.CreateExtractValue(Call, 0);
}

} // namespace neverd
