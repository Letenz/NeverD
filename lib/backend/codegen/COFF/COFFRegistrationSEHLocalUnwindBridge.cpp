//===- COFFRegistrationSEHLocalUnwindBridge.cpp - Native CRT ABI proof ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recheck the physical EBP, argument, clobber and exceptional-call protocol.

#include "COFFRegistrationSEHLocalUnwind.h"

#include "neverd/backend/RewriteSourceIdentity.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/TargetParser/Triple.h"

namespace neverd::coff_registration {

llvm::Expected<const llvm::Function *>
inspectSEHLocalUnwindBridge(const llvm::Function &Bridge,
                            RegistrationLocalUnwindContract::Kind Protocol) {
  using Kind = RegistrationLocalUnwindContract::Kind;
  const bool EH4 = Protocol == Kind::EH4;
  const auto &Target = Bridge.getParent()->getTargetTriple();
  const auto Pointer = [](llvm::Type *Type) {
    return Type->isPointerTy() && !Type->getPointerAddressSpace();
  };
  if ((Protocol != Kind::EH3 && !EH4) ||
      Target.getArch() != llvm::Triple::x86 || !Target.isOSWindows() ||
      !Bridge.hasLocalLinkage() || Bridge.size() != 1 || Bridge.isVarArg() ||
      !Bridge.getReturnType()->isVoidTy() ||
      Bridge.arg_size() != (EH4 ? 2u : 1u) ||
      Bridge.getCallingConv() != llvm::CallingConv::C ||
      !Bridge.hasFnAttribute(llvm::Attribute::NoInline) ||
      !Bridge.hasFnAttribute(llvm::Attribute::OptimizeNone) ||
      Bridge.getAttributes().getFnAttrs().getNumAttributes() != 2 ||
      Bridge.getAttributes().getRetAttrs().hasAttributes() ||
      Bridge.hasPersonalityFn() || Bridge.hasGC() || Bridge.hasPrefixData() ||
      Bridge.hasPrologueData() || Bridge.getAddressSpace())
    return llvm::createStringError(
        "local unwind bridge changed its private ABI");
  for (const auto &Argument : Bridge.args())
    if (!Pointer(Argument.getType()) || Bridge.getAttributes()
                                            .getParamAttrs(Argument.getArgNo())
                                            .hasAttributes())
      return llvm::createStringError("local unwind bridge changed an argument");
  auto BridgeIdentity = rewrite_source::getOriginalVA(Bridge);
  if (!BridgeIdentity)
    return BridgeIdentity.takeError();
  if (*BridgeIdentity || Bridge.getEntryBlock().size() != 2 ||
      Bridge.getEntryBlock().hasAddressTaken())
    return llvm::createStringError(
        "local unwind bridge gained unproved effects");
  const auto *Call =
      llvm::dyn_cast<llvm::CallInst>(&Bridge.getEntryBlock().front());
  const auto *Return =
      llvm::dyn_cast<llvm::ReturnInst>(&Bridge.getEntryBlock().back());
  const auto *Asm =
      Call ? llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand())
           : nullptr;
  if (!Call || !Return || Return->getReturnValue() || !Asm ||
      Call->arg_size() != (EH4 ? 3u : 2u) ||
      Call->getTailCallKind() != llvm::CallInst::TCK_NoTail ||
      Call->getCallingConv() != llvm::CallingConv::C ||
      Call->getAttributes().getFnAttrs().hasAttributes() ||
      Call->getAttributes().getRetAttrs().hasAttributes() ||
      Call->getNumOperandBundles() ||
      Call->getArgOperand(0) != Bridge.getArg(0) ||
      (EH4 && Call->getArgOperand(2) != Bridge.getArg(1)) ||
      !Asm->hasSideEffects() || Asm->isAlignStack() || !Asm->canThrow() ||
      Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
      !Call->getType()->isVoidTy() || Asm->getFunctionType()->isVarArg())
    return llvm::createStringError(
        "local unwind bridge changed its assembly call");
  for (unsigned I = 0; I != Call->arg_size(); ++I) {
    const auto Attributes = Call->getAttributes().getParamAttrs(I);
    if (I == 1 ? Attributes.getNumAttributes() != 1 ||
                     !Attributes.hasAttribute(llvm::Attribute::NonNull)
               : Attributes.hasAttributes())
      return llvm::createStringError(
          "local unwind bridge changed a call argument");
  }
  const llvm::StringRef Assembly =
      EH4 ? "pushl %ebp\n\tpushl $$-2\n\tpushl $0\n\tpushl $2\n\t"
            "movl $0, %ebp\n\taddl $$16, %ebp\n\tcalll *$1\n\t"
            "addl $$12, %esp\n\tpopl %ebp"
          : "pushl %ebp\n\tpushl $$-1\n\tpushl $0\n\t"
            "movl $0, %ebp\n\taddl $$16, %ebp\n\tcalll *$1\n\t"
            "addl $$8, %esp\n\tpopl %ebp";
  const llvm::StringRef Constraints =
      EH4 ? "r,{eax},{ecx},~{eax},~{ecx},~{edx},~{memory},~{dirflag},~{fpsr},~{"
            "flags}"
          : "r,{eax},~{eax},~{ecx},~{edx},~{memory},~{dirflag},~{fpsr},~{"
            "flags}";
  if (Asm->getAsmString() != Assembly ||
      Asm->getConstraintString() != Constraints)
    return llvm::createStringError(
        "local unwind bridge changed EBP or stack effects");
  const auto *Runtime = llvm::dyn_cast<llvm::Function>(Call->getArgOperand(1));
  if (!Runtime || !Runtime->isDeclaration() || !Runtime->hasExternalLinkage() ||
      Runtime->isVarArg() || !Runtime->getReturnType()->isVoidTy() ||
      Runtime->arg_size() != (EH4 ? 3u : 2u) ||
      Runtime->getCallingConv() != llvm::CallingConv::C ||
      !Runtime->getAttributes().isEmpty() ||
      !Pointer(Runtime->getArg(0)->getType()) ||
      (EH4 && !Pointer(Runtime->getArg(1)->getType())) ||
      !Runtime->getArg(EH4 ? 2 : 1)->getType()->isIntegerTy(32))
    return llvm::createStringError(
        "local unwind bridge changed its CRT runtime");
  auto Identity = rewrite_source::getOriginalVA(*Runtime);
  if (!Identity)
    return Identity.takeError();
  if (!*Identity || !**Identity || **Identity > UINT32_MAX)
    return llvm::createStringError(
        "local unwind bridge changed its source runtime");
  return Runtime;
}

llvm::Error
validateSEHLocalUnwindBridge(const llvm::Function &Bridge,
                             const RegistrationLocalUnwindContract &Source) {
  if (!Source.Target || Source.Target > UINT32_MAX || !Source.ImportSlot ||
      Source.ImportSlot > UINT32_MAX - 3 ||
      (Source.Indirect && Source.Target != Source.ImportSlot))
    return llvm::createStringError("local unwind source runtime is incomplete");
  auto Runtime = inspectSEHLocalUnwindBridge(Bridge, Source.Protocol);
  if (!Runtime)
    return Runtime.takeError();
  auto Identity = rewrite_source::getOriginalVA(**Runtime);
  if (!Identity)
    return Identity.takeError();
  if (!*Identity || **Identity != Source.Target ||
      (*Runtime)->hasDLLImportStorageClass() != Source.Indirect)
    return llvm::createStringError(
        "local unwind bridge changed its source runtime");
  return llvm::Error::success();
}

} // namespace neverd::coff_registration
