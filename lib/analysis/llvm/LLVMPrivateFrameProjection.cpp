//===- LLVMPrivateFrameProjection.cpp - Initialized local memory ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/analysis/LLVMPrivateFrameProjection.h"

#include "neverd/analysis/LLVMMemoryAnalysis.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace neverd::analysis {
namespace {
using namespace llvm;

struct Access {
  Instruction *Operation;
  Value *Object; // null selects the projected frame
  int64_t Offset;
  unsigned Bytes;
};

class Projection {
  Function &F;
  const LLVMPrivateFrameContract &Contract;
  const LLVMPrivateFrameLimits &Limits;
  const DataLayout &DL;
  LLVMPrivateFrameResult Result;
  unsigned Width = 0;
  SmallVector<BasicBlock *> Blocks;
  DenseMap<BasicBlock *, unsigned> BlockIndices;
  SmallVector<Access> Accesses;
  DenseMap<Instruction *, unsigned> FrameAccesses;
  DenseMap<Value *, uint64_t> Objects;

  bool charge(uint64_t Amount = 1) {
    if (Amount > Limits.MaxWork - Result.Work) {
      Result.Status = LLVMPrivateFrameResult::WorkLimitExceeded;
      Result.Diagnostic = "private-frame work budget exhausted";
      return false;
    }
    Result.Work += Amount;
    return true;
  }

  bool reject(const char *Message) {
    if (Result.Status != LLVMPrivateFrameResult::WorkLimitExceeded)
      Result.Diagnostic = Message;
    return false;
  }

  bool integralPointer(Type *Type) const {
    return Type->isPointerTy() && !Type->getPointerAddressSpace() &&
           !DL.isNonIntegralPointerType(Type);
  }

  std::optional<LLVMIntegerOffset> address(Value *Pointer) {
    if (!integralPointer(Pointer->getType()))
      return std::nullopt;
    APInt Offset(Width, 0);
    for (;;) {
      if (!charge())
        return std::nullopt;
      auto *GEP = dyn_cast<GEPOperator>(Pointer);
      if (!GEP)
        break;
      if (!charge(GEP->getNumIndices()))
        return std::nullopt;
      APInt Delta(Width, 0);
      if (!GEP->accumulateConstantOffset(DL, Delta))
        return std::nullopt;
      Offset += Delta;
      Pointer = GEP->getPointerOperand();
    }
    if (auto *Cast = dyn_cast<IntToPtrInst>(Pointer)) {
      auto Relation = splitLLVMIntegerOffset(
          Cast->getOperand(0), Offset,
          [&](uint64_t Amount) { return charge(Amount); });
      if (!Relation)
        return std::nullopt;
      if (auto *ToInt = dyn_cast<PtrToIntInst>(Relation->Root)) {
        if (!integralPointer(ToInt->getPointerOperand()->getType()))
          return std::nullopt;
        Relation->Root = ToInt->getPointerOperand();
      }
      return Relation;
    }
    return LLVMIntegerOffset{Pointer, std::move(Offset)};
  }

  bool contract() {
    Width = DL.getPointerSizeInBits(0);
    if ((Width != 32 && Width != 64) || Width != DL.getIndexSizeInBits(0) ||
        DL.getAllocaAddrSpace() || DL.isNonIntegralAddressSpace(0) ||
        F.isDeclaration() || F.isVarArg() || F.hasPersonalityFn() ||
        F.hasGC() || F.hasFnAttribute(Attribute::Naked) ||
        F.hasFnAttribute(Attribute::PresplitCoroutine))
      return reject("unsupported private-frame function or data layout");
    if (!Contract.Base || !Contract.Base->getType()->isIntegerTy(Width) ||
        Contract.Begin >= Contract.End ||
        (Width == 32 &&
         (Contract.Begin < INT32_MIN || Contract.End > INT32_MAX)) ||
        uint64_t(Contract.End) - uint64_t(Contract.Begin) >
            Limits.MaxFrameBytes)
      return reject("invalid private-frame base or declared extent");
    auto *Argument = dyn_cast<llvm::Argument>(Contract.Base);
    auto *Load = dyn_cast<LoadInst>(Contract.Base);
    if ((!Argument || Argument->getParent() != &F) &&
        (!Load || Load->getParent() != &F.getEntryBlock() || !Load->isSimple()))
      return reject("private-frame base is not a fixed entry value");
    if (!charge(Contract.OtherObjects.size()))
      return false;
    for (const auto &Object : Contract.OtherObjects) {
      if (!Object.Base || Object.Base->getParent() != &F ||
          !integralPointer(Object.Base->getType()) || !Object.Bytes ||
          Object.Bytes > uint64_t(Width == 32 ? INT32_MAX : INT64_MAX) ||
          !Objects.try_emplace(Object.Base, Object.Bytes).second)
        return reject("invalid or duplicate disjoint object contract");
    }
    return true;
  }

  bool collect() {
    uint64_t Instructions = 0;
    bool HasFrame = false;
    for (auto &Block : F) {
      if (Blocks.size() >= Limits.MaxBlocks || !charge())
        return reject("private-frame block limit exhausted");
      BlockIndices[&Block] = Blocks.size();
      Blocks.push_back(&Block);
      if (!isa<BranchInst, SwitchInst, ReturnInst>(Block.getTerminator()))
        return reject("unsupported private-frame control flow");
      if (!charge(Block.getTerminator()->getNumSuccessors()))
        return false;
      for (auto &I : Block) {
        if (++Instructions > Limits.MaxInstructions || !charge())
          return reject("private-frame instruction limit exhausted");
        if (I.hasMetadataOtherThanDebugLoc())
          return reject("private-frame instruction metadata requires proof");
        auto *Load = dyn_cast<LoadInst>(&I);
        auto *Store = dyn_cast<StoreInst>(&I);
        if (!Load && !Store) {
          if (isa<CallBase>(I) ? !isLLVMMemoryTransparentIntrinsic(I)
                               : I.mayReadOrWriteMemory() || I.mayThrow())
            return reject("private-frame analysis encountered an effect");
          continue;
        }
        if ((Load && !Load->isSimple()) || (Store && !Store->isSimple()))
          return reject("ordered private-frame memory is unsupported");
        Type *Type =
            Load ? Load->getType() : Store->getValueOperand()->getType();
        auto *Integer = dyn_cast<IntegerType>(Type);
        if (!Integer || Integer->getBitWidth() < 8 ||
            Integer->getBitWidth() > 128 || Integer->getBitWidth() % 8)
          return reject("unsupported private-frame access type");
        const unsigned Bytes = Integer->getBitWidth() / 8;
        auto Relation = address(Load ? Load->getPointerOperand()
                                     : Store->getPointerOperand());
        if (!Relation)
          return reject("unresolved private-frame memory address");
        const int64_t Offset = Relation->Offset.getSExtValue();
        if (Offset > INT64_MAX - Bytes)
          return reject("private-frame access extent overflows");
        const int64_t End = Offset + Bytes;
        if (Relation->Root == Contract.Base) {
          if (Offset < Contract.Begin || End > Contract.End)
            return reject("memory access exceeds the declared private frame");
          FrameAccesses[&I] = Accesses.size();
          Accesses.push_back({&I, nullptr, Offset, Bytes});
          Result.Begin = HasFrame ? std::min(Result.Begin, Offset) : Offset;
          Result.End = HasFrame ? std::max(Result.End, End) : End;
          HasFrame = true;
          Result.Loads += bool(Load);
          Result.Stores += bool(Store);
        } else {
          auto Object = Objects.find(Relation->Root);
          if (Object == Objects.end() || Offset < 0 ||
              uint64_t(End) > Object->second)
            return reject("memory access lacks a disjoint object contract");
          Accesses.push_back({&I, Relation->Root, Offset, Bytes});
        }
      }
    }
    return HasFrame || reject("function has no private-frame accesses");
  }

  bool initialized() {
    const uint64_t Bytes = uint64_t(Result.End) - uint64_t(Result.Begin);
    const uint64_t Words = (Bytes + 63) / 64;
    // Two vectors per block, plus one temporary vector. Charge every meet
    // by storage words and every scan by visited instructions/edges.
    if (Words > Limits.MaxDataflowBytes / 8 / (2 * Blocks.size() + 1) ||
        Bytes > std::numeric_limits<unsigned>::max())
      return reject("private-frame dataflow storage limit exhausted");
    SmallVector<bool> Reachable(Blocks.size(), false);
    SmallVector<unsigned> Pending{0};
    Reachable[0] = true;
    while (!Pending.empty()) {
      auto *Block = Blocks[Pending.pop_back_val()];
      if (!charge(1 + Block->getTerminator()->getNumSuccessors()))
        return false;
      for (auto *Successor : successors(Block)) {
        unsigned Index = BlockIndices.lookup(Successor);
        if (!Reachable[Index]) {
          Reachable[Index] = true;
          Pending.push_back(Index);
        }
      }
    }
    if (llvm::is_contained(Reachable, false))
      return reject("unreachable blocks require a separate projection");
    if (!charge(Words * (2 * Blocks.size() + 1)))
      return false;
    SmallVector<BitVector> In(Blocks.size(), BitVector(Bytes, true));
    SmallVector<BitVector> Out = In;
    bool Changed = true;
    while (Changed) {
      Changed = false;
      ++Result.Rounds;
      for (unsigned Index = 0; Index != Blocks.size(); ++Index) {
        BasicBlock *Block = Blocks[Index];
        if (!charge(Words))
          return false;
        BitVector State(Bytes, Index != 0);
        if (Index)
          for (auto *Predecessor : predecessors(Block)) {
            if (!charge(1 + Words))
              return false;
            State &= Out[BlockIndices.lookup(Predecessor)];
          }
        if (!charge(2 * Words))
          return false;
        if (In[Index] != State) {
          In[Index] = State;
          Changed = true;
        }
        for (auto &I : *Block) {
          if (!charge())
            return false;
          auto Found = FrameAccesses.find(&I);
          if (Found != FrameAccesses.end() && isa<StoreInst>(I)) {
            const Access &A = Accesses[Found->second];
            State.set(A.Offset - Result.Begin,
                      A.Offset - Result.Begin + A.Bytes);
          }
        }
        if (!charge(2 * Words))
          return false;
        if (Out[Index] != State) {
          Out[Index] = State;
          Changed = true;
        }
      }
    }
    for (unsigned Index = 0; Index != Blocks.size(); ++Index) {
      if (!charge(Words))
        return false;
      BitVector State = In[Index];
      for (auto &I : *Blocks[Index]) {
        if (!charge())
          return false;
        auto Found = FrameAccesses.find(&I);
        if (Found == FrameAccesses.end())
          continue;
        const Access &A = Accesses[Found->second];
        unsigned At = A.Offset - Result.Begin;
        if (!charge(A.Bytes))
          return false;
        if (isa<StoreInst>(I))
          State.set(At, At + A.Bytes);
        else
          for (unsigned J = 0; J != A.Bytes; ++J)
            if (!State[At + J])
              return reject("private-frame read is not definitely initialized");
      }
    }
    return true;
  }

  void apply() {
    IRBuilder<> Entry(&*F.getEntryBlock().getFirstInsertionPt());
    auto *Storage = ArrayType::get(
        Entry.getInt8Ty(), uint64_t(Result.End) - uint64_t(Result.Begin));
    auto *Local = Entry.CreateAlloca(Storage, nullptr, "private.frame");
    Local->setAlignment(Align(1));
    for (const Access &A : Accesses) {
      IRBuilder<> Builder(A.Operation);
      auto *Pointer = Builder.CreateGEP(
          Builder.getInt8Ty(), A.Object ? A.Object : Local,
          ConstantInt::get(Builder.getIntNTy(Width),
                           A.Object ? A.Offset : A.Offset - Result.Begin));
      if (auto *Load = dyn_cast<LoadInst>(A.Operation)) {
        Load->setOperand(0, Pointer);
        if (!A.Object)
          Load->setAlignment(Align(1));
      } else {
        auto *Store = cast<StoreInst>(A.Operation);
        Store->setOperand(1, Pointer);
        if (!A.Object)
          Store->setAlignment(Align(1));
      }
    }
  }

public:
  Projection(Function &F, const LLVMPrivateFrameContract &Contract,
             const LLVMPrivateFrameLimits &Limits)
      : F(F), Contract(Contract), Limits(Limits),
        DL(F.getParent()->getDataLayout()) {}

  LLVMPrivateFrameResult run() {
    if (contract() && collect() && initialized() &&
        charge(1 + 2 * Accesses.size())) {
      // All validation and construction charges precede the first mutation.
      apply();
      Result.Status = LLVMPrivateFrameResult::Projected;
      Result.Diagnostic = "initialized frame projected under caller contract";
    }
    return std::move(Result);
  }
};
} // namespace

LLVMPrivateFrameResult
projectLLVMPrivateFrame(llvm::Function &Function,
                        const LLVMPrivateFrameContract &Contract,
                        const LLVMPrivateFrameLimits &Limits) {
  if (!Function.getParent()) {
    LLVMPrivateFrameResult Result;
    Result.Diagnostic = "private-frame function has no module data layout";
    return Result;
  }
  return Projection(Function, Contract, Limits).run();
}
} // namespace neverd::analysis
