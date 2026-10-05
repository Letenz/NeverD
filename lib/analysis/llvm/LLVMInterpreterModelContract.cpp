//===- LLVMInterpreterModelContract.cpp - Scalar LLVM model ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LLVMInterpreterModelInternal.h"

namespace neverd::analysis::llvm_model {
namespace {
bool supportedLoopProperty(const llvm::MDNode *Property) {
  if (!Property || !Property->getNumOperands())
    return false;
  auto *Name = llvm::dyn_cast_or_null<llvm::MDString>(Property->getOperand(0));
  if (!Name)
    return false;
  if (Name->getString() == "llvm.loop.mustprogress" ||
      Name->getString() == "llvm.loop.unroll.disable")
    return Property->getNumOperands() == 1;
  if (Name->getString() != "llvm.loop.peeled.count" ||
      Property->getNumOperands() != 2)
    return false;
  // This is an unsigned history counter used by peeling heuristics. It
  // supplies neither an execution bound nor a definedness/termination fact.
  auto *Count = llvm::mdconst::dyn_extract_or_null<llvm::ConstantInt>(
      Property->getOperand(1));
  return Count && Count->getType()->isIntegerTy(32);
}
} // namespace

void Builder::validateContract() {
  llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> FunctionMetadata;
  F.getAllMetadata(FunctionMetadata);
  for (auto [Kind, Node] : FunctionMetadata)
    if (Kind != llvm::LLVMContext::MD_dbg)
      fail("unsupported function metadata contract");
  const llvm::MDNode *CommonTBAA = nullptr;
  for (auto &B : F)
    for (auto &I : B) {
      if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I);
          Load &&
          (!Load->getType()->isIntegerTy() || Load->getType()->isIntegerTy(1)))
        fail("noninteger or sub-byte memory representation unsupported");
      if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
          Store && (!Store->getValueOperand()->getType()->isIntegerTy() ||
                    Store->getValueOperand()->getType()->isIntegerTy(1)))
        fail("noninteger or sub-byte memory representation unsupported");
      llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> Metadata;
      I.getAllMetadataOtherThanDebugLoc(Metadata);
      for (auto [Kind, Node] : Metadata) {
        if (Kind == llvm::LLVMContext::MD_tbaa &&
            llvm::isa<llvm::LoadInst, llvm::StoreInst>(I)) {
          // A single mutable scalar tag cannot declare any two admitted
          // accesses noalias. Mixed tags need a separate checked alias model.
          if ((Node->getNumOperands() != 3 && Node->getNumOperands() != 4) ||
              Node->getOperand(0) != Node->getOperand(1))
            fail("unsupported TBAA access contract");
          auto *Offset = llvm::mdconst::dyn_extract<llvm::ConstantInt>(
              Node->getOperand(2));
          auto *Constant = Node->getNumOperands() == 4
                               ? llvm::mdconst::dyn_extract<llvm::ConstantInt>(
                                     Node->getOperand(3))
                               : nullptr;
          if (!Offset || !Offset->isZero() ||
              (Node->getNumOperands() == 4 &&
               (!Constant || !Constant->isZero())))
            fail("unproved TBAA constant or offset contract");
          if (CommonTBAA && CommonTBAA != Node)
            fail("mixed TBAA alias contracts unsupported");
          CommonTBAA = Node;
        } else if (Kind == llvm::LLVMContext::MD_loop && I.isTerminator()) {
          if (!Node->getNumOperands() || Node->getOperand(0) != Node)
            fail("unsupported loop metadata identity");
          for (unsigned N = 1; N < Node->getNumOperands(); ++N) {
            auto *Property =
                llvm::dyn_cast_or_null<llvm::MDNode>(Node->getOperand(N));
            if (!supportedLoopProperty(Property))
              fail("unsupported loop metadata contract");
          }
        } else
          fail("unsupported instruction metadata contract");
      }
    }
  if (F.getCallingConv() != llvm::CallingConv::C || F.hasGC() ||
      F.hasPersonalityFn() || F.hasPrefixData() || F.hasPrologueData())
    fail("unsupported function execution contract");
  for (auto A : F.getAttributes().getFnAttrs()) {
    if (A.isStringAttribute()) {
      auto K = A.getKindAsString();
      if (K != "min-legal-vector-width" && K != "no-trapping-math" &&
          K != "stack-protector-buffer-size" && K != "target-cpu" &&
          K != "target-features" && K != "tune-cpu" &&
          !(ScalarArguments && K == "frame-pointer"))
        fail("unsupported string function attribute obligation");
      continue;
    }
    switch (A.getKindAsEnum()) {
    case llvm::Attribute::NoInline:
    case llvm::Attribute::AlwaysInline:
    case llvm::Attribute::InlineHint:
    case llvm::Attribute::OptimizeNone:
    case llvm::Attribute::OptimizeForSize:
    case llvm::Attribute::MinSize:
      if (!ScalarArguments)
        fail("unsupported function optimization attribute obligation");
      break;
    // The admitted subset has no calls, synchronization, deallocation,
    // recursion or exceptional operations. Output loops have checked ranks.
    case llvm::Attribute::NoFree:
    case llvm::Attribute::NoRecurse:
    case llvm::Attribute::NoSync:
    case llvm::Attribute::NoUnwind:
    case llvm::Attribute::MustProgress:
    case llvm::Attribute::WillReturn:
    case llvm::Attribute::UWTable:
    case llvm::Attribute::Memory:
      break;
    default:
      fail("unsupported function attribute obligation: " + A.getAsString());
    }
  }
  auto Effects = F.getMemoryEffects();
  for (auto &B : F)
    for (auto &I : B) {
      const llvm::Value *Pointer = nullptr;
      bool Write = false;
      if (auto *L = llvm::dyn_cast<llvm::LoadInst>(&I))
        Pointer = L->getPointerOperand();
      if (auto *S = llvm::dyn_cast<llvm::StoreInst>(&I)) {
        Pointer = S->getPointerOperand();
        Write = true;
      }
      if (!Pointer)
        continue;
      auto Kind = StateOffsets.count(Pointer) ? llvm::IRMemLocation::ArgMem
                                              : llvm::IRMemLocation::Other;
      auto Permission = Effects.getModRef(Kind);
      if (Write ? !llvm::isModSet(Permission) : !llvm::isRefSet(Permission))
        fail("function memory attribute excludes an executed access");
    }
  for (auto A : F.getAttributes().getRetAttrs())
    if (A.isStringAttribute() ||
        (A.getKindAsEnum() != llvm::Attribute::Range &&
         A.getKindAsEnum() != llvm::Attribute::NoUndef))
      fail("unsupported return attribute obligation");
  for (auto &Arg : F.args())
    for (auto A : F.getAttributes().getParamAttrs(Arg.getArgNo())) {
      if (ScalarArguments && (!A.isEnumAttribute() ||
                              A.getKindAsEnum() != llvm::Attribute::NoUndef))
        fail("unsupported scalar argument attribute obligation");
      if (!((A.isEnumAttribute() &&
             (A.getKindAsEnum() == llvm::Attribute::NoUndef ||
              (A.getKindAsEnum() == llvm::Attribute::NoFree))) ||
            (!A.isStringAttribute() &&
             A.getKindAsEnum() == llvm::Attribute::Initializes) ||
            (A.getAsString() == "captures(none)")))
        fail("unsupported argument attribute obligation");
    }
}

} // namespace neverd::analysis::llvm_model
