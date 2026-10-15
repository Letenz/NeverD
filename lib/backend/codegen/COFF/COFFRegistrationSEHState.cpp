//===- COFFRegistrationSEHState.cpp - Callback state proof ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Close source state writes against their exact LLVM callback occurrences.

#include "COFFRegistrationSEHState.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/backend/llvm/X86RegistrationSEHJump.h"
#include "neverd/ir/med/X86RegistrationSEHStateStore.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicsX86.h"

namespace neverd::coff_registration {

llvm::Expected<std::set<const llvm::CallBase *>>
bindSEHCallbackStateWrites(const MedFunc &Source, const llvm::Function &Parent,
                           const std::map<int, SourceSegment> &Segments,
                           llvm::ArrayRef<const llvm::Function *> Functions) {
  const auto Expected = getX86RegistrationSEHStateStores(Source);
  if (!Expected)
    return rejectIR("callback state writes have incomplete source receipts");
  std::set<const llvm::CallBase *> Result;
  std::map<std::pair<va_t, int>, size_t> Occurrences;
  for (size_t I = 0; I != Expected->size(); ++I)
    if (!Occurrences
             .emplace(std::make_pair((*Expected)[I].Source->Address,
                                     (*Expected)[I].Source->OpSeq),
                      I)
             .second)
      return rejectIR("callback state-write source identity is ambiguous");
  size_t Work = 0;
  for (const auto *Function : Functions)
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR("callback state writes exceed their work budget");
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
        const bool StateWrite =
            Call && Call->getCalledFunction() &&
            Call->getCalledFunction()->getName() == "llvm.x86.seh.setstate";
        if (StateWrite && !Result.count(Call))
          return rejectIR("physical state write has no adjacent source store");
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        if (!MD)
          continue;
        const auto Address = metadataInteger(*MD, 1, 64);
        const auto Seq = metadataInteger(*MD, 2, 32);
        if (!Address || !Seq || *Seq > INT_MAX)
          return rejectIR("callback state source operation is malformed");
        const auto Found = Occurrences.find({*Address, int(*Seq)});
        if (Found == Occurrences.end())
          continue;
#ifndef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES
        return rejectIR("LLVM lacks physical callback state-write receipts");
#else
        const auto &Bound = (*Expected)[Found->second];
        const auto &Receipt = *Bound.Source;
        const auto Owner = Segments.find(Receipt.BlockId);
        const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
        const auto *Write = llvm::dyn_cast_or_null<llvm::CallInst>(next(I));
        const auto *Frame =
            getX86RegistrationSEHJumpParentFrame(*Function, Parent);
        if (!Store || Function == &Parent || Owner == Segments.end() ||
            !Owner->second.Enter ||
            Owner->second.Enter->getFunction() != Function ||
            MD->getNumOperands() != 5 ||
            metadataInteger(*MD, 0, 64) != Source.Entry ||
            metadataInteger(*MD, 3, 32) != uint32_t(Receipt.BlockId) ||
            metadataInteger(*MD, 4, 8) != 1 || Receipt.Width != 4 ||
            Store->isAtomic() || !Store->isVolatile() ||
            Store->getAlign() != llvm::Align(1) ||
            integer(Store->getValueOperand(), 32) != Receipt.Value ||
            Receipt.Transitions.empty() ||
            llvm::any_of(Receipt.Transitions,
                         [&](const auto &Transition) {
                           return uint32_t(Transition.second) != Receipt.Value;
                         }) ||
            !Write ||
            Write->getIntrinsicID() != llvm::Intrinsic::x86_seh_setstate ||
            Write->arg_size() != 8 || Write->getNumOperandBundles() ||
            Write->getCallingConv() != llvm::CallingConv::C ||
            Write->isTailCall() || !Write->use_empty() ||
            !Write->getAttributes().isEmpty() ||
            Write->getArgOperand(0) != &Parent || !Frame ||
            Write->getArgOperand(1) != Frame ||
            integer(Write->getArgOperand(2), 32) != Receipt.Value ||
            integer(Write->getArgOperand(7), 64) != Found->second + 1 ||
            !Result.insert(Write).second)
          return rejectIR(
              "callback state write changed its source or frame ABI");
        std::array<uint64_t, 4> Digest{};
        if (int32_t(Receipt.Value) >= 0) {
          const auto Token = windows_eh_semantics::getSEHScopeSemanticToken(
              *Source.ExceptionMetadata, Arch::X86, Receipt.Value);
          if (!Token)
            return rejectIR("callback state write has no source scope token");
          Digest = Token->Digest;
        } else if (int32_t(Receipt.Value) !=
                   *Source.ExceptionMetadata->Registration->SeededTryLevel)
          return rejectIR("callback state write changed its inactive sentinel");
        for (unsigned J = 0; J != Digest.size(); ++J)
          if (integer(Write->getArgOperand(3 + J), 64) != Digest[J])
            return rejectIR("callback state write changed its scope digest");
#endif
      }
  if (Result.size() != Expected->size())
    return rejectIR("callback source state write has no physical update");
  return Result;
}

llvm::Expected<std::set<const llvm::InvokeInst *>> validateSEHRetainedScopes(
    const MedFunc &Source, const llvm::Function &Parent,
    const llvm::BasicBlock &SourceEntry,
    const std::map<uint32_t, const llvm::BasicBlock *> &Unwinds) {
  const auto Expected = getX86RegistrationSEHRetainedScopes(Source);
  if (!Expected)
    return rejectIR(
        "source scopes have no closed parent/callback reachability");
  std::set<const llvm::InvokeInst *> Result;
  auto CheckClosure = [&]() -> llvm::Error {
    size_t Work = 0;
    for (const auto &Part : Parent)
      for (const auto &I : Part) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR("scope retention closure exceeds its work budget");
        if (I.getMetadata(
                windows_eh_md::RegistrationScopeRetentionAttachment) &&
            !Result.count(llvm::dyn_cast<llvm::InvokeInst>(&I)))
          return rejectIR("scope retention has an extra entry edge");
      }
    return llvm::Error::success();
  };
  if (&Parent.getEntryBlock() == &SourceEntry) {
    if (!Expected->empty())
      return rejectIR("callback-selected rows have no retention entry chain");
    if (auto Error = CheckClosure())
      return std::move(Error);
    return Result;
  }
  const auto *Previous = &Parent.getEntryBlock();
  const auto *Branch =
      llvm::dyn_cast<llvm::UncondBrInst>(Previous->getTerminator());
  if (!Branch)
    return rejectIR("machine entry bypasses its checked registration block");
  const auto *Block = Branch->getSuccessor(0);
  for (uint32_t Scope : *Expected) {
    const auto *Invoke =
        llvm::dyn_cast<llvm::InvokeInst>(Block->getTerminator());
    const auto *MD =
        Invoke ? Invoke->getMetadata(
                     windows_eh_md::RegistrationScopeRetentionAttachment)
               : nullptr;
    if (Block->size() != 1 || Block->getSinglePredecessor() != Previous ||
        Block->getParent() != &Parent || !Invoke || !MD ||
        Invoke->getIntrinsicID() != llvm::Intrinsic::donothing ||
        Invoke->arg_size() || Invoke->getNumOperandBundles() ||
        Invoke->getCallingConv() != llvm::CallingConv::C ||
        !Invoke->getAttributes().isEmpty() || MD->getNumOperands() != 2 ||
        metadataInteger(*MD, 0, 64) != Source.Entry ||
        metadataInteger(*MD, 1, 32) != Scope || !Unwinds.count(Scope) ||
        Invoke->getUnwindDest() != Unwinds.at(Scope) ||
        !Result.insert(Invoke).second)
      return rejectIR("callback-only scope changed its retention edge");
    Previous = Block;
    Block = Invoke->getNormalDest();
  }
  if (Block != &SourceEntry)
    return rejectIR("scope retention chain bypasses the source entry");
  if (auto Error = CheckClosure())
    return std::move(Error);
  return Result;
}

} // namespace neverd::coff_registration
