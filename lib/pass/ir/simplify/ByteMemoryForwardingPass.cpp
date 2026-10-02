//===- ByteMemoryForwardingPass.cpp - Forward overlapping stack bytes
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/simplify/ByteMemoryForwardingPass.h"

#include "neverd/pass/ir/simplify/SymSimplifyPass.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/Operator.h"

#include <algorithm>
#include <optional>

namespace neverd {
namespace {

struct Address {
  llvm::AllocaInst *Object;
  uint64_t Offset;
  uint64_t Size;
};

struct StoredByte {
  llvm::StoreInst *Writer;
  unsigned Index;
};

bool charge(uint64_t &Used, uint64_t Amount, uint64_t Limit,
            ByteMemoryForwardingResult &Result) {
  if (Amount > Limit - Used) {
    Result.BudgetExhausted = true;
    return false;
  }
  Used += Amount;
  return true;
}

std::optional<Address> address(llvm::Value *Pointer, const llvm::DataLayout &DL,
                               const ByteMemoryForwardingOptions &Options,
                               ByteMemoryForwardingResult &Result) {
  if (Pointer->getType()->getPointerAddressSpace() != 0 ||
      DL.isNonIntegralPointerType(Pointer->getType()))
    return std::nullopt;
  const unsigned Width = DL.getIndexSizeInBits(0);
  // A narrower GEP index can wrap independently of the pointer representation.
  if ((Width != 32 && Width != 64) || Width != DL.getPointerSizeInBits(0))
    return std::nullopt;
  llvm::APInt Offset(Width, 0);
  for (;;) {
    if (!charge(Result.AddressSteps, 1, Options.MaxAddressSteps, Result))
      return std::nullopt;
    if (auto *Object = llvm::dyn_cast<llvm::AllocaInst>(Pointer)) {
      auto *Bytes = llvm::dyn_cast<llvm::ArrayType>(Object->getAllocatedType());
      auto *Count = llvm::dyn_cast<llvm::ConstantInt>(Object->getArraySize());
      if (!Object->isStaticAlloca() || Object->getAddressSpace() != 0 ||
          !Bytes || !Bytes->getElementType()->isIntegerTy(8) || !Count ||
          !Count->isOne() || Offset.isNegative())
        return std::nullopt;
      const uint64_t Size = Bytes->getNumElements();
      if (!Size || Size > 65536 || Offset.getZExtValue() >= Size)
        return std::nullopt;
      return Address{Object, Offset.getZExtValue(), Size};
    }
    // In particular, never strip inttoptr, addrspacecast, PHI or select. A
    // common underlying allocation alone does not establish an exact address.
    auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(Pointer);
    if (!GEP || !charge(Result.AddressSteps, GEP->getNumIndices(),
                        Options.MaxAddressSteps, Result))
      return std::nullopt;
    llvm::APInt Delta(Width, 0);
    if (!GEP->accumulateConstantOffset(DL, Delta))
      return std::nullopt;
    bool Overflow = false;
    Offset = Offset.sadd_ov(Delta, Overflow);
    if (Overflow)
      return std::nullopt;
    Pointer = GEP->getPointerOperand();
  }
}

std::optional<unsigned> scalarBytes(llvm::Type *Type) {
  auto *Integer = llvm::dyn_cast<llvm::IntegerType>(Type);
  if (!Integer || Integer->getBitWidth() < 8 || Integer->getBitWidth() > 128 ||
      Integer->getBitWidth() % 8)
    return std::nullopt;
  return Integer->getBitWidth() / 8;
}

bool needsSnapshot(llvm::Value *Value, llvm::StoreInst *Store) {
  // Restrict the optional definedness query to leaves with a constant-time
  // answer. Recursing into a PHI can visit arbitrarily many incoming edges,
  // even with ValueTracking's depth limit. An extra freeze is conservative;
  // ordinary LLVM cleanup can remove it after proving the expression defined.
  if (!llvm::isa<llvm::ConstantInt, llvm::Argument, llvm::FreezeInst>(Value))
    return true;
  return !llvm::isGuaranteedNotToBeUndefOrPoison(Value, nullptr, Store);
}

} // namespace

ByteMemoryForwardingResult
ByteMemoryForwardingPass::forward(llvm::Function &F,
                                  ByteMemoryForwardingOptions Options) {
  ByteMemoryForwardingResult Result;
  if (F.isDeclaration() || F.hasFnAttribute(kObfuscatedFnAttr))
    return Result;
  const llvm::DataLayout &DL = F.getParent()->getDataLayout();
  for (llvm::BasicBlock &BB : F) {
    llvm::DenseMap<llvm::AllocaInst *, llvm::DenseMap<uint64_t, StoredByte>>
        Memory;
    uint64_t TrackedBytes = 0;
    auto Invalidate = [&] {
      Memory.clear();
      TrackedBytes = 0;
    };
    for (auto It = BB.begin(); It != BB.end();) {
      llvm::Instruction *I = &*It++;
      if (!charge(Result.Instructions, 1, Options.MaxInstructions, Result))
        return Result;
      auto *Store = llvm::dyn_cast<llvm::StoreInst>(I);
      auto *Load = llvm::dyn_cast<llvm::LoadInst>(I);
      if (!Store && !Load) {
        if (llvm::isa<llvm::CallBase, llvm::FenceInst>(I) ||
            I->mayWriteToMemory())
          Invalidate();
        continue;
      }
      auto Width = scalarBytes(Store ? Store->getValueOperand()->getType()
                                     : Load->getType());
      if (!Width || (Store && !Store->isSimple()) ||
          (Load && !Load->isSimple())) {
        if (Store || (Load && !Load->isSimple()))
          Invalidate();
        continue;
      }
      auto A = address(Store ? Store->getPointerOperand()
                             : Load->getPointerOperand(),
                       DL, Options, Result);
      if (!A || *Width > A->Size - A->Offset) {
        if (Store)
          Invalidate();
        continue;
      }
      auto Found = Memory.find(A->Object);
      if (Store) {
        uint64_t Added = 0;
        for (unsigned J = 0; J != *Width; ++J)
          Added +=
              Found == Memory.end() || !Found->second.contains(A->Offset + J);
        if (Added > Options.MaxTrackedBytes - TrackedBytes) {
          Result.BudgetExhausted = true;
          Invalidate();
          continue;
        }
        auto &Bytes = Memory[A->Object];
        for (unsigned J = 0; J != *Width; ++J)
          Bytes[A->Offset + J] = {Store,
                                  DL.isLittleEndian() ? J : *Width - 1 - J};
        TrackedBytes += Added;
        Result.PeakTrackedBytes =
            std::max(Result.PeakTrackedBytes, TrackedBytes);
        continue;
      }
      if (Found == Memory.end())
        continue;
      llvm::SmallVector<StoredByte, 16> Parts;
      for (unsigned J = 0; J != *Width; ++J) {
        auto Part = Found->second.find(A->Offset + J);
        if (Part == Found->second.end())
          break;
        Parts.push_back(Part->second);
      }
      if (Parts.size() != *Width)
        continue;

      bool UsesFit = true;
      for (const llvm::Use &Use : Load->uses()) {
        (void)Use;
        // Pay for this preflight walk and the corresponding RAUW visit.
        if (!charge(Result.UseSteps, 2, Options.MaxUseSteps, Result)) {
          UsesFit = false;
          break;
        }
      }
      if (!UsesFit)
        continue;

      llvm::SmallPtrSet<llvm::StoreInst *, 16> Seen;
      llvm::SmallVector<llvm::StoreInst *, 16> Snapshots;
      uint64_t Cost = 0;
      for (const StoredByte &Part : Parts) {
        llvm::Value *Value = Part.Writer->getValueOperand();
        Cost += 3 + !Value->getType()->isIntegerTy(8) + (*Width > 1);
        if (Seen.insert(Part.Writer).second &&
            needsSnapshot(Value, Part.Writer))
          Snapshots.push_back(Part.Writer);
      }
      Cost += Snapshots.size();
      if (!Options.AllowStoreSnapshots && !Snapshots.empty())
        continue;
      // Reserve the entire reconstruction before touching either the store or
      // load. NoFolder makes this an exact count, independent of constants.
      if (!charge(Result.NewInstructions, Cost, Options.MaxNewInstructions,
                  Result))
        continue;
      for (llvm::StoreInst *Writer : Snapshots) {
        llvm::IRBuilder<llvm::NoFolder> AtStore(Writer);
        Writer->setOperand(0, AtStore.CreateFreeze(Writer->getValueOperand(),
                                                   "byte.snapshot"));
        ++Result.FrozenStores;
      }
      llvm::IRBuilder<llvm::NoFolder> B(Load);
      llvm::Value *Value = llvm::ConstantInt::get(Load->getType(), 0);
      for (unsigned J = 0; J != *Width; ++J) {
        const StoredByte Part = Parts[J];
        llvm::Value *Slice =
            B.CreateLShr(Part.Writer->getValueOperand(), Part.Index * 8);
        Slice = B.CreateTruncOrBitCast(Slice, B.getInt8Ty());
        Slice = B.CreateZExtOrTrunc(Slice, Load->getType());
        Slice =
            B.CreateShl(Slice, (DL.isLittleEndian() ? J : *Width - 1 - J) * 8);
        Value = B.CreateOr(Value, Slice);
      }
      Load->replaceAllUsesWith(Value);
      Load->eraseFromParent();
      ++Result.ForwardedLoads;
    }
  }
  return Result;
}

llvm::PreservedAnalyses
ByteMemoryForwardingPass::run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &) {
  return forward(F, Options).ForwardedLoads ? llvm::PreservedAnalyses::none()
                                            : llvm::PreservedAnalyses::all();
}

} // namespace neverd
