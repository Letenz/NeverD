//===- StateSourceBank.cpp - Bounded state-bank effects and values --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Share bank address admission and conservative interprocedural write facts.

#include "StateSourceBank.h"

#include "neverd/pipeline/Limits.h"
#include "neverd/pipeline/StateSourceRecovery.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Errc.h"

#include <set>
#include <tuple>

namespace neverd::state_source {
namespace {
llvm::Error invalid(llvm::StringRef Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "state source contract: " + Message);
}
} // namespace

// Exact pointer-sized arithmetic only. Loading a pointer from the bank is a
// guest access, not an address of the bank. Never chase that load as a base.
std::optional<Offsets> bankOffsets(llvm::Value *V, llvm::Value *Base,
                                   const llvm::DataLayout &DL,
                                   unsigned &Budget) {
  if (!Budget)
    return {};
  --Budget;
  if (V == Base)
    return Offsets{{0}, false};
  if (llvm::isa<llvm::LoadInst, llvm::Argument, llvm::Constant>(V))
    return Offsets{{}, true};
  auto Next = [&](llvm::Value *X) { return bankOffsets(X, Base, DL, Budget); };
  if (auto *C = llvm::dyn_cast<llvm::CastInst>(V)) {
    if (C->getOpcode() == llvm::Instruction::BitCast)
      return Next(C->getOperand(0));
    if (C->getOpcode() == llvm::Instruction::PtrToInt &&
        C->getType()->getIntegerBitWidth() == DL.getPointerSizeInBits(0))
      return Next(C->getOperand(0));
    if (C->getOpcode() == llvm::Instruction::IntToPtr &&
        C->getType()->getPointerAddressSpace() == 0 &&
        C->getOperand(0)->getType()->getIntegerBitWidth() ==
            DL.getPointerSizeInBits(0))
      return Next(C->getOperand(0));
    return {};
  }
  auto Shift = [](std::optional<Offsets> Values, int64_t Amount) {
    if (Values)
      for (auto &O : Values->Bank)
        O += Amount;
    return Values;
  };
  if (auto *G = llvm::dyn_cast<llvm::GetElementPtrInst>(V)) {
    llvm::APInt Offset(DL.getIndexSizeInBits(0), 0);
    if (G->getAddressSpace() == 0 && G->accumulateConstantOffset(DL, Offset) &&
        Offset.getSignificantBits() <= 32)
      return Shift(Next(G->getPointerOperand()), Offset.getSExtValue());
    return {};
  }
  if (auto *B = llvm::dyn_cast<llvm::BinaryOperator>(V)) {
    if (B->getOpcode() != llvm::Instruction::Add &&
        B->getOpcode() != llvm::Instruction::Sub)
      return {};
    auto *K = llvm::dyn_cast<llvm::ConstantInt>(B->getOperand(1));
    auto *Source = B->getOperand(0);
    if (!K && B->getOpcode() == llvm::Instruction::Add) {
      K = llvm::dyn_cast<llvm::ConstantInt>(B->getOperand(0));
      Source = B->getOperand(1);
    }
    if (K && K->getValue().getSignificantBits() <= 32)
      return Shift(Next(Source), B->getOpcode() == llvm::Instruction::Sub
                                     ? -K->getSExtValue()
                                     : K->getSExtValue());
    return {};
  }
  std::vector<llvm::Value *> Arms;
  if (auto *P = llvm::dyn_cast<llvm::PHINode>(V))
    for (auto &A : P->incoming_values())
      Arms.push_back(A.get());
  if (auto *S = llvm::dyn_cast<llvm::SelectInst>(V))
    Arms = {S->getTrueValue(), S->getFalseValue()};
  if (Arms.empty())
    return {};
  Offsets Result;
  for (auto *A : Arms) {
    auto Values = Next(A);
    if (!Values)
      return {};
    Result.Guest |= Values->Guest;
    for (auto O : Values->Bank)
      if (!llvm::is_contained(Result.Bank, O))
        Result.Bank.push_back(O);
    if (Result.Bank.size() > ::neverd::limits::pipeline::
                                 kStateSourceBankBankOffsetsResultBankSizeLimit)
      return {};
  }
  return Result;
}
std::optional<Offsets> bankOffsets(llvm::Value *V, llvm::Value *Base,
                                   const llvm::DataLayout &DL) {
  unsigned Budget =
      ::neverd::limits::pipeline::kStateSourceBankBankOffsetsBudget;
  return bankOffsets(V, Base, DL, Budget);
}
std::optional<int64_t> displacement(llvm::Value *V, llvm::Value *Base,
                                    const llvm::DataLayout &DL) {
  auto Values = bankOffsets(V, Base, DL);
  if (Values && !Values->Guest && Values->Bank.size() == 1)
    return Values->Bank.front();
  return {};
}

// Admit bounded bank accesses before attaching isolation facts. Address
// observation or escape must not turn an ambiguous access into guest memory.
llvm::Error validateBank(llvm::Function &F, unsigned Bytes) {
  auto *Base = F.getArg(0);
  const auto &DL = F.getParent()->getDataLayout();
  std::vector<llvm::Value *> Work{Base};
  std::set<llvm::Value *> Seen{Base};
  while (!Work.empty()) {
    auto *V = Work.back();
    Work.pop_back();
    for (auto &Use : V->uses()) {
      auto *I = llvm::dyn_cast<llvm::Instruction>(Use.getUser());
      if (!I)
        return invalid("bank address escapes the function");
      llvm::Type *T = nullptr;
      if (auto *L = llvm::dyn_cast<llvm::LoadInst>(I)) {
        if (L->isVolatile() || L->isAtomic())
          return invalid("ordered bank access");
        T = L->getType();
      } else if (auto *S = llvm::dyn_cast<llvm::StoreInst>(I)) {
        if (Use.getOperandNo() != 1 || S->isVolatile() || S->isAtomic())
          return invalid("bank address escapes or has ordered access");
        T = S->getValueOperand()->getType();
      } else if (auto *Call = llvm::dyn_cast<llvm::CallInst>(I)) {
        auto Offset = displacement(V, Base, DL);
        if (Use.getOperandNo() != 0 || Offset != 0 ||
            !V->getType()->isPointerTy() || !Call->getType()->isIntegerTy(64) ||
            Call->isMustTailCall() || Call->hasOperandBundles() ||
            Call->getCallingConv() != llvm::CallingConv::C ||
            Call->getFunctionType()->isVarArg())
          return invalid("unsupported bank call boundary");
        if (auto *Callee = Call->getCalledFunction())
          if (Callee->getFunctionType() != Call->getFunctionType())
            return invalid("inconsistent gateway ABI");
        continue;
      } else if (bankOffsets(I, Base, DL)) {
        if (Seen.insert(I).second)
          Work.push_back(I);
        continue;
      } else {
        std::string Detail;
        llvm::raw_string_ostream OS(Detail);
        OS << "bank address is observed or has a dynamic offset in "
           << F.getName() << ": ";
        I->print(OS);
        return invalid(Detail);
      }
      auto Offsets = bankOffsets(V, Base, DL);
      if (!Offsets || DL.getTypeStoreSize(T).isScalable())
        return invalid("bank access has an unbounded address or width");
      for (auto Offset : Offsets->Bank)
        if (Offset < 0 || uint64_t(Offset) > Bytes ||
            DL.getTypeStoreSize(T).getFixedValue() > Bytes - Offset)
          return invalid("bank access outside declared storage");
    }
  }
  return llvm::Error::success();
}

namespace {
using Slot = std::tuple<unsigned, unsigned, llvm::Type *>;
using Facts = std::map<Slot, llvm::Value *>;

llvm::Value *memoryPointer(llvm::Instruction &I) {
  if (auto *L = llvm::dyn_cast<llvm::LoadInst>(&I))
    return L->getPointerOperand();
  if (auto *S = llvm::dyn_cast<llvm::StoreInst>(&I))
    return S->getPointerOperand();
  return nullptr;
}
llvm::Type *memoryType(llvm::Instruction &I) {
  if (auto *S = llvm::dyn_cast<llvm::StoreInst>(&I))
    return S->getValueOperand()->getType();
  return I.getType();
}
bool eligible(llvm::Function &F, unsigned Bytes) {
  if (!F.hasExactDefinition() || F.arg_empty() ||
      !F.getArg(0)->getType()->isPointerTy() || F.hasPersonalityFn() ||
      F.size() >
          ::neverd::limits::pipeline::kStateSourceBankEligibleFSizeLimit ||
      F.getInstructionCount() >
          ::neverd::limits::pipeline::
              kStateSourceBankEligibleFGetInstructionCountLimit)
    return false;
  if (auto E = validateBank(F, Bytes)) {
    llvm::consumeError(std::move(E));
    return false;
  }
  for (auto &I : llvm::instructions(F)) {
    if (auto *L = llvm::dyn_cast<llvm::LoadInst>(&I); L && !L->isSimple())
      return false;
    if (auto *S = llvm::dyn_cast<llvm::StoreInst>(&I); S && !S->isSimple())
      return false;
    if (llvm::isa<llvm::InvokeInst, llvm::CallBrInst>(&I))
      return false;
    if (auto *C = llvm::dyn_cast<llvm::CallInst>(&I))
      if (C->canReturnTwice() || C->isInlineAsm() || C->isConvergent() ||
          C->isMustTailCall() || C->hasOperandBundles())
        return false;
  }
  return true;
}
bool bankCall(llvm::CallInst &C, llvm::Value *Bank) {
  return !C.arg_empty() && displacement(C.getArgOperand(0), Bank,
                                        C.getModule()->getDataLayout()) == 0;
}
llvm::BitVector storeWrites(llvm::StoreInst &S, llvm::Value *Bank,
                            unsigned Bytes) {
  llvm::BitVector Result(Bytes);
  const auto &DL = S.getModule()->getDataLayout();
  auto Addresses = bankOffsets(S.getPointerOperand(), Bank, DL);
  // validateBank already rejects unbounded bank-derived uses. Other
  // pointers are guest memory under the explicit isolation assertion.
  if (!Addresses)
    return Result;
  auto Width = DL.getTypeStoreSize(S.getValueOperand()->getType());
  for (int64_t Offset : Addresses->Bank) {
    if (Width.isScalable() || Offset < 0 || uint64_t(Offset) > Bytes ||
        Width.getFixedValue() > Bytes - Offset)
      return llvm::BitVector(Bytes, true);
    Result.set(Offset, Offset + Width.getFixedValue());
  }
  return Result;
}
std::optional<Slot> exactSlot(llvm::Instruction &I, llvm::Value *Bank,
                              unsigned Bytes) {
  auto *Pointer = memoryPointer(I);
  if (!Pointer)
    return {};
  const auto &DL = I.getModule()->getDataLayout();
  auto Offset = displacement(Pointer, Bank, DL);
  auto *Type = memoryType(I);
  if (!Offset || *Offset < 0 || uint64_t(*Offset) > Bytes ||
      !(Type->isIntegerTy() || Type->isPointerTy()))
    return {};
  auto Width = DL.getTypeStoreSize(Type);
  if (Width.isScalable() || Width.getFixedValue() > Bytes - *Offset ||
      (Width != 1 && Width != 2 && Width != 4 && Width != 8) ||
      (Type->isIntegerTy() && Type->getIntegerBitWidth() != Width * 8))
    return {};
  return Slot{unsigned(*Offset), unsigned(Width.getFixedValue()), Type};
}
void kill(Facts &Available, const llvm::BitVector &Writes) {
  for (auto I = Available.begin(); I != Available.end();) {
    auto [Offset, Width, Type] = I->first;
    if (Writes.find_first_in(Offset, Offset + Width) >= 0)
      I = Available.erase(I);
    else
      ++I;
  }
}

void forward(llvm::Function &F, unsigned Bytes, const CallWrites &Effects,
             StateSourceRecovery &R) {
  llvm::DominatorTree DT(F);
  auto *Bank = F.getArg(0);
  std::map<const llvm::BasicBlock *, Facts> In, Out;
  std::map<llvm::Value *, llvm::Value *> Replacements;
  std::vector<llvm::StoreInst *> Redundant;
  unsigned Work = 0;
  auto Resolve = [&](llvm::Value *V) {
    for (;;) {
      auto I = Replacements.find(V);
      if (I == Replacements.end())
        break;
      V = I->second;
    }
    return V;
  };
  auto Transfer = [&](llvm::BasicBlock &BB, Facts &Values, unsigned Collect) {
    for (auto &I : BB) {
      if (++Work > ::neverd::limits::pipeline::
                       kStateSourceBankForwardTransferWorkLimit ||
          Values.size() > ::neverd::limits::pipeline::
                              kStateSourceBankForwardTransferValuesSizeLimit)
        return false;
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&I)) {
        kill(Values, Effects.at(Call));
        continue;
      }
      auto Key = exactSlot(I, Bank, Bytes);
      if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
        auto Existing = Key ? Values.find(*Key) : Values.end();
        auto *V = Store->getValueOperand();
        if (Collect == 2 && Existing != Values.end() &&
            Resolve(Existing->second) == Resolve(V) &&
            !llvm::isa<llvm::UndefValue>(Resolve(V)))
          Redundant.push_back(Store);
        kill(Values, storeWrites(*Store, Bank, Bytes));
        if (Key)
          Values[*Key] = V;
      } else if (Key && llvm::isa<llvm::LoadInst>(I)) {
        auto [At, Added] = Values.emplace(*Key, &I);
        if (Collect == 1 && !Added && At->second != &I &&
            !llvm::isa<llvm::UndefValue>(At->second) &&
            DT.dominates(At->second, &I))
          Replacements.emplace(&I, At->second);
      }
    }
    return true;
  };
  // Unvisited predecessors are top, not empty. Iteration then removes facts
  // invalidated by backedges, retaining only a single common reaching value.
  bool Changed;
  do {
    Changed = false;
    for (auto &BB : F) {
      if (!DT.isReachableFromEntry(&BB))
        continue;
      std::optional<Facts> Values;
      if (&BB == &F.getEntryBlock())
        Values.emplace();
      else
        for (auto *P : llvm::predecessors(&BB)) {
          auto Known = Out.find(P);
          if (Known == Out.end())
            continue;
          if (!Values) {
            Values = Known->second;
            continue;
          }
          for (auto I = Values->begin(); I != Values->end();) {
            auto Other = Known->second.find(I->first);
            if (Other == Known->second.end() || Other->second != I->second)
              I = Values->erase(I);
            else
              ++I;
          }
        }
      if (!Values)
        continue;
      In[&BB] = *Values;
      if (!Transfer(BB, *Values, 0))
        return;
      auto Existing = Out.find(&BB);
      if (Existing == Out.end() || Existing->second != *Values) {
        Out[&BB] = std::move(*Values);
        Changed = true;
      }
    }
  } while (Changed);
  for (unsigned Collect : {1u, 2u})
    for (auto &BB : F)
      if (auto I = In.find(&BB); I != In.end()) {
        auto Values = I->second;
        if (!Transfer(BB, Values, Collect))
          return;
      }
  // No mutation before both the CFG fixed point and complete construction
  // budget succeed. Resolve chains before deleting any old definition.
  for (auto [Load, Value] : Replacements)
    Load->replaceAllUsesWith(Resolve(Value));
  for (auto [Load, Value] : Replacements)
    llvm::cast<llvm::Instruction>(Load)->eraseFromParent();
  for (auto *Store : Redundant)
    Store->eraseFromParent();
  R.ForwardedStateLoads += Replacements.size();
  R.RedundantStateStores += Redundant.size();
}
} // namespace

void refineBankAccesses(llvm::Module &M, StateSourceRecovery &R,
                        const CallWrites &GatewayWrites) {
  const unsigned Bytes = R.Contract.StateBytes;
  if (!R.Contract.LocalizeBank ||
      Bytes > ::neverd::limits::pipeline::
                  kStateSourceBankRefineBankAccessesBytesLimit ||
      M.size() > ::neverd::limits::pipeline::
                     kStateSourceBankRefineBankAccessesMSizeLimit)
    return;
  std::map<llvm::Function *, llvm::BitVector> Writes;
  std::map<llvm::Function *, std::vector<llvm::Function *>> Dependencies;
  std::map<llvm::Function *, bool> Eligible;
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    bool Valid = Eligible[&F] = eligible(F, Bytes);
    Writes.emplace(&F, llvm::BitVector(Bytes, !Valid));
  }
  auto CallMask = [&](llvm::CallInst &Call, llvm::Value *Bank) {
    if (!bankCall(Call, Bank))
      return llvm::BitVector(Bytes);
    if (auto E = GatewayWrites.find(&Call); E != GatewayWrites.end())
      return E->second;
    auto F = Writes.find(Call.getCalledFunction());
    return F == Writes.end() ? llvm::BitVector(Bytes, true) : F->second;
  };
  for (auto &[F, Valid] : Eligible) {
    if (!Valid)
      continue;
    for (auto &I : llvm::instructions(F)) {
      if (auto *S = llvm::dyn_cast<llvm::StoreInst>(&I))
        Writes.at(F) |= storeWrites(*S, F->getArg(0), Bytes);
      if (auto *C = llvm::dyn_cast<llvm::CallInst>(&I)) {
        Writes.at(F) |= CallMask(*C, F->getArg(0));
        if (bankCall(*C, F->getArg(0)) && !GatewayWrites.count(C) &&
            Writes.count(C->getCalledFunction()))
          Dependencies[F].push_back(C->getCalledFunction());
      }
    }
  }
  // A monotone may-write union covers recursion and mutually recursive SCCs.
  // Unknown/over-budget callees invalidate every byte, never just their ABI
  // return register. The caller's complete return bank remains observable.
  unsigned Work = 0;
  bool Changed;
  do {
    Changed = false;
    for (auto &[F, Callees] : Dependencies)
      for (auto *C : Callees) {
        if (++Work > ::neverd::limits::pipeline::
                         kStateSourceBankRefineBankAccessesWorkLimit)
          return;
        auto Before = Writes.at(F);
        Writes.at(F) |= Writes.at(C);
        Changed |= Before != Writes.at(F);
      }
  } while (Changed);
  CallWrites Effects;
  for (auto &[F, Valid] : Eligible)
    if (Valid)
      for (auto &I : llvm::instructions(F))
        if (auto *C = llvm::dyn_cast<llvm::CallInst>(&I)) {
          auto Mask = CallMask(*C, F->getArg(0));
          if (bankCall(*C, F->getArg(0)) && !Mask.all())
            ++R.BoundedStateCalls;
          Effects.emplace(C, std::move(Mask));
        }
  for (auto &[F, Valid] : Eligible)
    if (Valid)
      forward(*F, Bytes, Effects, R);
}
} // namespace neverd::state_source
