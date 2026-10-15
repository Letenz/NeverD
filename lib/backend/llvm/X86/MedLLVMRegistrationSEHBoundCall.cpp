//===- MedLLVMRegistrationSEHBoundCall.cpp - Preserved helper arguments ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Project preserved x86 SEH helper arguments onto the generated frame.

#include "MedLLVMRegistrationSEHJump.h"

#include "neverd/backend/RewriteSourceIdentity.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

namespace neverd::x86_registration {

bool prepareSEHBoundCall(const llvm::CallInst &Call,
                         const RegistrationSEHBoundCall &Source,
                         const llvm::AllocaInst &Frame, uint64_t EntrySP) {
  const auto *Callee = Call.getCalledFunction();
  if (!Callee || !Callee->isDeclaration() ||
      Callee->getFunctionType() !=
          getX86RegistrationSEHBoundType(Source, Call.getContext()) ||
      Call.getCallingConv() != llvm::CallingConv::C ||
      Call.arg_size() != Source.Arguments.size() || Call.isTailCall() ||
      Call.getNumOperandBundles())
    return false;
  auto Identity = rewrite_source::getOriginalVA(*Callee);
  if (!Identity) {
    llvm::consumeError(Identity.takeError());
    return false;
  }
  if (!*Identity || **Identity != Source.TargetVA)
    return false;
  const auto Bytes = Frame.getAllocationSize(Call.getModule()->getDataLayout());
  if (!Bytes || Bytes->isScalable())
    return false;
  for (const auto &Arg : Source.Arguments) {
    using Kind = RegistrationSEHBoundArgument::Kind;
    if (Arg.ArgumentKind != Kind::Scalar && Arg.ArgumentKind != Kind::Object)
      continue;
    const int64_t Offset = int64_t(EntrySP) - 4 + Arg.Offset;
    if (Offset < 0 || uint64_t(Offset) + 4 > Bytes->getFixedValue())
      return false;
  }
  return true;
}

std::map<llvm::CallInst *, llvm::CallInst *>
emitSEHBoundCalls(const SEHJumpPlan &Plan, llvm::AllocaInst &Frame,
                  uint64_t EntrySP,
                  const std::map<va_t, llvm::AllocaInst *> &Buffers) {
  std::map<llvm::CallInst *, llvm::CallInst *> Result;
  for (const auto &[Call, Source] : Plan.BoundCalls) {
    llvm::IRBuilder<> B(Call);
    llvm::SmallVector<llvm::Value *, 8> Arguments;
    for (const auto &Arg : Source.Source->Arguments) {
      using Kind = RegistrationSEHBoundArgument::Kind;
      switch (Arg.ArgumentKind) {
      case Kind::Unused:
        Arguments.push_back(B.getInt32(0));
        break;
      case Kind::JumpBuffer:
        Arguments.push_back(Buffers.at(Arg.Capture));
        break;
      case Kind::Object:
      case Kind::Scalar: {
        auto *Slot =
            B.CreateInBoundsGEP(B.getInt8Ty(), &Frame,
                                B.getInt32(int64_t(EntrySP) - 4 + Arg.Offset));
        if (Arg.ArgumentKind == Kind::Object)
          Arguments.push_back(Slot);
        else {
          auto *Value = B.CreateLoad(B.getInt32Ty(), Slot);
          Value->setAlignment(llvm::Align(1));
          Value->setVolatile(true);
          Arguments.push_back(Value);
        }
        break;
      }
      }
    }
    auto *Replacement = B.CreateCall(Call->getCalledFunction(), Arguments);
    if (!Source.Source->HasOrdinaryReturn)
      Replacement->addFnAttr(llvm::Attribute::NoReturn);
    Replacement->copyMetadata(*Call);
    Result.emplace(Call, Replacement);
  }
  return Result;
}

} // namespace neverd::x86_registration
