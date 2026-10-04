//===- LLVMInterpreterModelInitialization.cpp - Initialization contracts --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMInterpreterModelInternal.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/IR/CFG.h"

namespace neverd::analysis::llvm_model {
void Builder::validateInitialization() {
  const auto Attribute =
      F.getAttributes().getParamAttr(0, llvm::Attribute::Initializes);
  if (!Attribute.isValid())
    return;
  const auto Ranges = Attribute.getValueAsConstantRangeList();
  if (Ranges.empty())
    fail("empty state initializes contract");
  using Bits = llvm::BitVector;
  Bits Claimed(StateBytes);
  uint64_t LastEnd = 0;
  bool First = true;
  for (const auto &Range : Ranges) {
    work();
    if (Range.getBitWidth() != 64)
      fail("state initializes range must have 64-bit endpoints");
    const auto Begin = Range.getLower().getZExtValue();
    const auto End = Range.getUpper().getZExtValue();
    if (Begin >= End || End > StateBytes || (!First && Begin <= LastEnd))
      fail("state initializes ranges must be separated and inside the state "
           "object");
    work(End - Begin);
    for (uint64_t I = Begin; I != End; ++I)
      Claimed.set(I);
    First = false;
    LastEnd = End;
  }

  enum class Kind { Read, Write, Return };
  struct Access {
    Kind Operation;
    Bits Bytes;
  };
  struct BlockState {
    Bits Stores, In, Out;
    std::vector<Access> Accesses;
  };
  std::map<const llvm::BasicBlock *, BlockState> Flow;
  for (const auto &B : F) {
    work();
    auto &State = Flow[&B];
    State.Stores.resize(StateBytes);
    State.Out = Claimed;
    for (const auto &I : B) {
      work();
      const llvm::Value *Pointer = nullptr;
      llvm::Type *Type = nullptr;
      uint64_t Alignment = 1;
      bool Write = false;
      if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
        Pointer = Load->getPointerOperand();
        Type = Load->getType();
        Alignment = Load->getAlign().value();
      } else if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
        // Special writes never establish the attribute's initialization.
        // The ordinary memory importer rejects these effects as well.
        if (Store->isAtomic() || Store->isVolatile())
          fail("unsupported initializes store effect");
        Pointer = Store->getPointerOperand();
        Type = Store->getValueOperand()->getType();
        Alignment = Store->getAlign().value();
        Write = true;
      } else if (llvm::isa<llvm::ReturnInst>(I)) {
        State.Accesses.push_back({Kind::Return, {}});
        continue;
      }
      // Guest storage is disjoint from the state object. Its writes cannot
      // initialize state bytes. Unknown state-derived pointers still fail
      // the ordinary pointer/value importer; they are not granted credit.
      if (!Pointer || !StateOffsets.count(Pointer))
        continue;
      auto Slot = stateSlot(Pointer, bytes(Type), Alignment);
      Bits Mask(StateBytes);
      work(Slot.Size);
      for (uint64_t J = 0; J != Slot.Size; ++J)
        Mask.set(Slot.Offset + J);
      Mask &= Claimed;
      if (Mask.none())
        continue;
      State.Accesses.push_back({Write ? Kind::Write : Kind::Read, Mask});
      if (Write)
        State.Stores |= Mask;
    }
  }

  // Must analysis: entry is always empty; all other blocks start at top.
  // Only a converged predecessor intersection may justify a read or return.
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (const auto &B : F) {
      work();
      Bits In(StateBytes);
      if (&B != &F.getEntryBlock()) {
        In = Claimed;
        for (const auto *Predecessor : llvm::predecessors(&B)) {
          work();
          In &= Flow.at(Predecessor).Out;
        }
      }
      auto &State = Flow.at(&B);
      auto Out = In;
      Out |= State.Stores;
      Changed |= Out != State.Out;
      State.In = In;
      State.Out = Out;
    }
  }
  for (const auto &B : F) {
    work();
    const auto &State = Flow.at(&B);
    auto Initialized = State.In;
    for (const auto &Access : State.Accesses) {
      work();
      switch (Access.Operation) {
      case Kind::Write:
        Initialized |= Access.Bytes;
        break;
      case Kind::Read:
        if (Access.Bytes.test(Initialized))
          fail("state initializes contract may read before initialization");
        break;
      case Kind::Return:
        if (Claimed.test(Initialized))
          fail("state initializes contract may leave undef bytes at return");
        break;
      }
    }
  }
}
} // namespace neverd::analysis::llvm_model
