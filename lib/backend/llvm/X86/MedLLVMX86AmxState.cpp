//===- MedLLVMX86AmxState.cpp - Native AMX bank operations -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Hardware owns complete tile state and publishes restart progress directly.
/// Only an architectural ZMM row result enters ordinary LLVM value SSA.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86AmxAsm.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ErrorHandling.h"

#include <vector>
namespace neverd {
llvm::Value *MedLLVMEmitter::emitX86AmxState(const MedOp &Op,
                                             llvm::IRBuilder<> &B) {
  auto S = x86AmxMedShape(Op, TargetArch);
  if (TargetArch != Arch::X64 || !x86AmxShapeIsValid(S))
    llvm::report_fatal_error("invalid AMX architectural bank contract");
  const auto D = *S.Descriptor;
  const auto K = x86AmxOperation(D);
  const auto P = x86AmxAsm(D, S.MemoryAddressSpace);
  if (!P)
    llvm::report_fatal_error("invalid AMX native assembly contract");
  std::vector<llvm::Value *> Args;
  llvm::AllocaInst *Row = nullptr;
  if (P->Memory) {
    auto *Base =
        resolveAtomicMemoryPtr(Op.Inputs[1], x86AmxIsTileMemory(K) ? 1 : 64,
                               B.getInt8Ty(), B, S.MemoryAddressSpace);
    Args.push_back(Base);
    if (x86AmxIsTileMemory(K))
      Args.push_back(getRawSegmentOffset(Op.Inputs[3], B));
  } else if (P->Row) {
    auto &EntryBlock = B.GetInsertBlock()->getParent()->getEntryBlock();
    llvm::IRBuilder<> Entry(&EntryBlock, EntryBlock.begin());
    Row = Entry.CreateAlloca(sizeToType(X86AmxRowBytes), nullptr, "amx_row");
    Args.push_back(Row);
    Args.push_back(getRawSegmentOffset(Op.Inputs[3], B));
  }
  std::vector<llvm::Type *> Types;
  for (unsigned I = 0; I < Args.size(); ++I) {
    auto *T = sizeToType(P->InputBytes[I]);
    if (P->Row && I == 0)
      T = Args[I]->getType();
    if (Args[I]->getType() != T)
      Args[I] = Args[I]->getType()->isPointerTy()
                    ? B.CreatePtrToInt(Args[I], T)
                    : B.CreateZExtOrTrunc(Args[I], T);
    Types.push_back(T);
  }
  auto *Asm =
      llvm::InlineAsm::get(llvm::FunctionType::get(B.getVoidTy(), Types, false),
                           P->Text, P->Constraints, true);
  B.CreateCall(Asm, Args);
  return Row ? B.CreateLoad(sizeToType(X86AmxRowBytes), Row) : nullptr;
}
} // namespace neverd
