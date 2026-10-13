//===- LLVMCStackAllocation.cpp - Executable C stack allocation ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve LLVM allocation count, layout stride and instruction placement.
//===----------------------------------------------------------------------===//

#include "LLVMCStackAllocation.h"

#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"

#include <limits>
#include <stdexcept>

namespace neverd::llvmc {
namespace {

constexpr llvm::StringLiteral AllocationMarker = "neverd.c.stack-allocation";

using BlockSet = llvm::SmallPtrSet<const llvm::BasicBlock *, 16>;

/// The blocks one call can run more than once: every block on a CFG cycle,
/// self-loops and unwind edges included.
BlockSet repeatingBlocks(const llvm::Function &Function) {
  BlockSet Blocks;
  if (Function.isDeclaration())
    return Blocks;
  for (auto It = llvm::scc_begin(&Function); !It.isAtEnd(); ++It)
    if (It.hasCycle())
      Blocks.insert(It->begin(), It->end());
  return Blocks;
}

bool needsRuntimeAllocation(const llvm::AllocaInst &Allocation,
                            const BlockSet &Repeating) {
  const auto *Count =
      llvm::dyn_cast<llvm::ConstantInt>(Allocation.getArraySize());
  if (!Count || !Count->isOne())
    return true;
  // One object outside the entry block lives until the function returns, as
  // a function-scope C object does, unless its block runs again and needs a
  // fresh object each time.
  return !Allocation.isStaticAlloca() &&
         (Allocation.isUsedWithInAlloca() ||
          Repeating.contains(Allocation.getParent()));
}

CStackAllocation allocationShape(const llvm::AllocaInst &Allocation) {
  const auto &Layout = Allocation.getModule()->getDataLayout();
  const unsigned Bits = Layout.getPointerSizeInBits(0);
  if ((Bits != 32 && Bits != 64) || Allocation.getAddressSpace() != 0 ||
      Layout.getAllocaAddrSpace() != 0 || Allocation.isUsedWithInAlloca() ||
      Allocation.isSwiftError() ||
      Allocation.getArraySize()->getType()->getIntegerBitWidth() > Bits)
    throw std::runtime_error(
        "C projection has an unsupported stack allocation contract");
  const auto Size = Layout.getTypeAllocSize(Allocation.getAllocatedType());
  if (Size.isScalable())
    throw std::runtime_error(
        "C projection has scalable stack allocation storage");
  const uint64_t Stride = Size.getFixedValue();
  const uint64_t Alignment = Allocation.getAlign().value();
  const uint64_t Max = Bits == 64 ? UINT64_MAX : UINT32_MAX;
  if (Stride > Max || Alignment - 1 > Max)
    throw std::runtime_error(
        "C projection stack allocation layout exceeds pointer width");
  if (const auto *Count =
          llvm::dyn_cast<llvm::ConstantInt>(Allocation.getArraySize()))
    if (Stride && Count->getZExtValue() > (Max - (Alignment - 1)) / Stride)
      throw std::runtime_error(
          "C projection stack allocation extent overflows");
  return {Stride, Alignment, Bits};
}

} // namespace

void validateCStackAllocations(const llvm::Module &Module,
                               const llvm::Function *Only) {
  for (const auto &Function : Module) {
    if (Function.hasFnAttribute(AllocationMarker))
      throw std::runtime_error(
          "C projection input contains a private stack allocation marker");
    if (Only && &Function != Only)
      continue;
    const BlockSet Repeating = repeatingBlocks(Function);
    for (const auto &Block : Function)
      for (const auto &Instruction : Block) {
        if (const auto *Allocation =
                llvm::dyn_cast<llvm::AllocaInst>(&Instruction))
          if (needsRuntimeAllocation(*Allocation, Repeating) ||
              Allocation->isSwiftError() || Allocation->isUsedWithInAlloca() ||
              Allocation->getAddressSpace() != 0)
            (void)allocationShape(*Allocation);
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
          if (Call->getIntrinsicID() == llvm::Intrinsic::stacksave ||
              Call->getIntrinsicID() == llvm::Intrinsic::stackrestore)
            throw std::runtime_error(
                "C projection cannot preserve explicit stack save/restore");
      }
  }
}

void lowerCStackAllocations(llvm::Module &Module, const llvm::Function *Only) {
  llvm::SmallVector<llvm::AllocaInst *, 16> Work;
  for (auto &Function : Module) {
    if (Only && &Function != Only)
      continue;
    const BlockSet Repeating = repeatingBlocks(Function);
    for (auto &Block : Function)
      for (auto &Instruction : Block)
        if (auto *Allocation = llvm::dyn_cast<llvm::AllocaInst>(&Instruction))
          if (needsRuntimeAllocation(*Allocation, Repeating))
            Work.push_back(Allocation);
  }
  if (Work.empty())
    return;
  const unsigned Bits = Module.getDataLayout().getPointerSizeInBits(0);
  auto *CountType = llvm::IntegerType::get(Module.getContext(), Bits);
  auto *PointerType = llvm::PointerType::get(Module.getContext(), 0);
  auto *Type = llvm::FunctionType::get(
      PointerType, {CountType, CountType, CountType}, false);
  // Function::Create gives collisions a distinct name. Recognition uses the
  // private marker, not a user-controlled function name.
  auto *Allocator =
      llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                             "nd_c_stack_allocation", Module);
  Allocator->addFnAttr(AllocationMarker);
  for (auto *Allocation : Work) {
    const auto Shape = allocationShape(*Allocation);
    llvm::IRBuilder<> Builder(Allocation);
    auto *Count = Builder.CreateZExt(Allocation->getArraySize(), CountType);
    auto *Call = Builder.CreateCall(
        Allocator, {Count, llvm::ConstantInt::get(CountType, Shape.Stride),
                    llvm::ConstantInt::get(CountType, Shape.Alignment)});
    Call->takeName(Allocation);
    Call->setDebugLoc(Allocation->getDebugLoc());
    Allocation->replaceAllUsesWith(Call);
    Allocation->eraseFromParent();
  }
}

bool isCStackAllocationFunction(const llvm::Function &Function) {
  return Function.hasFnAttribute(AllocationMarker);
}

std::optional<CStackAllocation> cStackAllocation(const llvm::CallBase &Call) {
  const auto *Function = Call.getCalledFunction();
  if (!Function || !isCStackAllocationFunction(*Function))
    return std::nullopt;
  const unsigned Bits =
      Call.getModule()->getDataLayout().getPointerSizeInBits(0);
  const auto *Stride =
      Call.arg_size() == 3
          ? llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(1))
          : nullptr;
  const auto *Alignment =
      Call.arg_size() == 3
          ? llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(2))
          : nullptr;
  if (!Function->isDeclaration() || Function->isVarArg() ||
      (Bits != 32 && Bits != 64) || Function->arg_size() != 3 ||
      !llvm::isa<llvm::CallInst>(Call) || Call.hasOperandBundles() ||
      Call.getCallingConv() != llvm::CallingConv::C ||
      !Call.getType()->isPointerTy() ||
      Call.getType()->getPointerAddressSpace() != 0 || !Stride || !Alignment ||
      !Call.getArgOperand(0)->getType()->isIntegerTy(Bits) ||
      !Stride->getType()->isIntegerTy(Bits) ||
      !Alignment->getType()->isIntegerTy(Bits) ||
      !Alignment->getValue().isPowerOf2())
    throw std::runtime_error(
        "C projection has a malformed private stack allocation");
  return CStackAllocation{Stride->getZExtValue(), Alignment->getZExtValue(),
                          Bits};
}

} // namespace neverd::llvmc
