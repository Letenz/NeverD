//===- COFFRegistrationSEHCall.cpp - Dynamic call receipts ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationSEHCall.h"

#include "COFFNativeEHProvenance.h"
#include "COFFRegistrationIRProof.h"

#include "neverd/backend/RewriteSourceIdentity.h"

namespace neverd::coff_registration {

bool isDynamicSEHCall(const llvm::CallBase &Call) {
#ifndef LLVM_NEVERD_X86_REGISTRATION_SEH_DYNAMIC_CALLS
  return false;
#else
  const auto Attribute =
      llvm::mc_rewrite::RewriteWinX86SEHInheritStateAttribute;
  const auto Marker = Call.getAttributes().getFnAttr(Attribute);
  const auto *CI = llvm::dyn_cast<llvm::CallInst>(&Call);
  const auto *Callee = Call.getCalledFunction();
  return CI && !llvm::isa<llvm::IntrinsicInst>(CI) &&
         CI->getTailCallKind() == llvm::CallInst::TCK_NoTail &&
         !CI->isInlineAsm() && !CI->getNumOperandBundles() &&
         Marker.isValid() && Marker.getValueAsString().empty() &&
         CI->getAttributes().hasFnAttr(llvm::Attribute::NoInline) &&
         !CI->getFunction()->hasFnAttribute(Attribute) &&
         (!Callee || !Callee->hasFnAttribute(Attribute));
#endif
}

const llvm::CallInst *dynamicSEHCall(const llvm::CallInst &Anchor) {
  const auto P = coff_native_eh::parseNativeEHProvenance(Anchor);
  const auto *Call = llvm::dyn_cast_or_null<llvm::CallInst>(next(Anchor));
  if (!P ||
      P->Model != windows_eh_md::NativeProvenanceModel::X86RegistrationSEH ||
      P->Role != windows_eh_md::NativeProvenanceRole::RegistrationDynamicCall ||
      P->AuxVA || P->Flags || Anchor.arg_size() ||
      Anchor.getNumOperandBundles() != 1 || !Call || !isDynamicSEHCall(*Call) ||
      !matchesSourceOperation(*Call, P->FunctionVA, P->SourceVA, 2))
    return nullptr;
  const auto *MD =
      Call->getMetadata(windows_eh_md::RegistrationOperationAttachment);
  if (metadataInteger(*MD, 2, 32) != P->Clause ||
      metadataInteger(*MD, 3, 32) != P->Region)
    return nullptr;
  auto Owner = rewrite_source::getOriginalVA(*Call->getFunction());
  if (!Owner) {
    llvm::consumeError(Owner.takeError());
    return nullptr;
  }
  return *Owner && **Owner == P->FunctionVA ? Call : nullptr;
}

} // namespace neverd::coff_registration
