//===- LLVMCFrameLayout.h - Stable synthetic-frame display context --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Operator.h"

#include <optional>

namespace neverd::llvmc {

inline constexpr char CapturedFrameBase[] = "neverd.llvmc.frame-base";

inline std::optional<uint64_t>
syntheticFrameBaseOffset(const llvm::AllocaInst &Frame,
                         const llvm::DataLayout &Layout) {
  const auto *Array = llvm::dyn_cast<llvm::ArrayType>(Frame.getAllocatedType());
  if (!Array || !Array->getElementType()->isIntegerTy(8))
    return std::nullopt;
  if (const auto *Metadata = Frame.getMetadata(CapturedFrameBase)) {
    if (Metadata->getNumOperands() != 1)
      return std::nullopt;
    const auto *Offset =
        llvm::mdconst::dyn_extract<llvm::ConstantInt>(Metadata->getOperand(0));
    if (Offset && Offset->getBitWidth() <= 64 &&
        Offset->getZExtValue() <= Array->getNumElements())
      return Offset->getZExtValue();
    return std::nullopt;
  }
  std::optional<uint64_t> Found;
  for (const auto *User : Frame.users()) {
    const auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(User);
    if (!GEP)
      continue;
    bool IsBase = GEP->getName() == "frame_end";
    for (const auto *Use : GEP->users())
      if (const auto *Address = llvm::dyn_cast<llvm::PtrToIntInst>(Use))
        IsBase |= Address->getName().starts_with("rsp_init");
    llvm::APInt Offset(64, 0);
    if (!IsBase || !GEP->accumulateConstantOffset(Layout, Offset))
      continue;
    const uint64_t Value = Offset.getZExtValue();
    if (Value > Array->getNumElements() || (Found && *Found != Value))
      return std::nullopt;
    Found = Value;
  }
  return Found;
}

} // namespace neverd::llvmc
