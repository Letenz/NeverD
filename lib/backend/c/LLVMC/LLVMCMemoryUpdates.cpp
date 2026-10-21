//===- LLVMCMemoryUpdates.cpp - Scalar memory updates ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Combine a captured read/modify/write in the C view without hiding effects.
//===----------------------------------------------------------------------===//

#include "../UnalignedMemory.h"
#include "LLVMCWriter.h"

#include "neverd/backend/Limits.h"

#include "llvm/Analysis/ValueTracking.h"

namespace neverd {

namespace {
llvm::StringRef updateOperator(unsigned Opcode) {
  switch (Opcode) {
  case llvm::Instruction::Add:
    return "+=";
  case llvm::Instruction::Sub:
    return "-=";
  case llvm::Instruction::And:
    return "&=";
  case llvm::Instruction::Or:
    return "|=";
  case llvm::Instruction::Xor:
    return "^=";
  default:
    return {};
  }
}
const llvm::LoadInst *updateLoad(const llvm::BinaryOperator &Update) {
  const auto *Type = Update.getType();
  // Unsigned C carriers preserve these modular operations, including the
  // integer promotions of byte and halfword operands.
  if (updateOperator(Update.getOpcode()).empty() ||
      (!Type->isIntegerTy(8) && !Type->isIntegerTy(16) &&
       !Type->isIntegerTy(32) && !Type->isIntegerTy(64)))
    return nullptr;
  const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Update.getOperand(0));
  const auto *Constant =
      llvm::dyn_cast<llvm::ConstantInt>(Update.getOperand(1));
  if ((!Load || !Constant) && Update.isCommutative()) {
    Load = llvm::dyn_cast<llvm::LoadInst>(Update.getOperand(1));
    Constant = llvm::dyn_cast<llvm::ConstantInt>(Update.getOperand(0));
  }
  if (!Load || !Constant || !Load->isSimple() || !Load->hasOneUse() ||
      Load->getType() != Type || Load->getParent() != Update.getParent() ||
      Load->getPointerAddressSpace() != 0)
    return nullptr;
  return Load;
}

bool effectFreeBetween(const llvm::Instruction &From,
                       const llvm::Instruction &To, unsigned &Work) {
  if (From.getParent() != To.getParent() || !From.comesBefore(&To))
    return false;
  for (const auto *I = From.getNextNode(); I != &To; I = I->getNextNode()) {
    if (!Work || I->mayReadOrWriteMemory() || I->mayHaveSideEffects() ||
        llvm::isa<llvm::CallBase>(I))
      return false;
    --Work;
  }
  return true;
}
} // namespace

void LLVMCWriter::planRawMemoryUpdates(llvm::Function &Fn) {
  if (!UnalignedTypesWritten)
    return;
  auto RawLoad =
      [&](const llvm::BinaryOperator &Update) -> const llvm::LoadInst * {
    const auto *Load = updateLoad(Update);
    if (!Load || ValueTexts.count(&Update) || ValueTexts.count(Load) ||
        Analysis.DeadFrameStores.count(&Update) ||
        Analysis.DeadFrameStores.count(Load) ||
        Analysis.ForwardedLoads.count(Load))
      return nullptr;
    const auto *Pointer = Load->getPointerOperand();
    const auto *Base = llvm::getUnderlyingObject(Pointer);
    // Named objects and local homes already have their own C type/lifetime
    // projections. This rule owns only the raw scalar memory fallback.
    const auto Size = llvmAccessSize(Load->getType());
    if ((!llvm::isa<llvm::Argument>(Base) &&
         !llvm::isa<llvm::IntToPtrInst>(Base)) ||
        imageDataVA(Pointer) || typedLoadAccess(*Load) ||
        frameSlotAccess(Pointer, Size, false, false))
      return nullptr;
    return Load;
  };
  for (auto &Block : Fn) {
    for (auto &Instruction : Block) {
      const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction);
      if (!Store || !Store->isSimple() ||
          Store->getPointerAddressSpace() != 0 ||
          Analysis.DeadFrameStores.count(Store))
        continue;
      const auto *Update =
          llvm::dyn_cast<llvm::BinaryOperator>(Store->getValueOperand());
      if (!Update || Update->getParent() != &Block)
        continue;
      const auto *Load = RawLoad(*Update);
      unsigned Work = ::neverd::limits::backend::kLLVMCMemoryUpdateAddressWork;
      if (!Load || Load->getPointerOperand() != Store->getPointerOperand() ||
          !effectFreeBetween(*Load, *Store, Work))
        continue;
      bool Valid = true;
      // If the updated value survives, capture the compound expression at
      // the store. It must dominate every other use, including PHI edges.
      // An earlier observer must still see the old ordering and is refused.
      for (const auto &Use : Update->uses()) {
        if (Use.getUser() == Store)
          continue;
        if (!Work || !Dominators.dominates(Store, Use)) {
          Valid = false;
          break;
        }
        --Work;
      }
      if (!Valid)
        continue;
      RawMemoryUpdates.emplace(Store, Update);
      RawMemoryUpdateInputs.insert(Load);
      RawMemoryUpdateInputs.insert(Update);
      Analysis.Inlinable.insert(Load);
      if (Update->hasOneUse()) {
        Analysis.Inlinable.insert(Update);
      } else {
        Analysis.Inlinable.erase(Update);
        MaterializedExpressions.insert(Update);
      }
    }
  }
  // When an intermediate observer prevents joining the store, the single
  // read can still belong to its already-materialized arithmetic statement.
  // Pin that result in place: further expression inlining could defer the
  // read across effects or put it behind a conditional expression.
  for (auto &Block : Fn) {
    for (auto &Instruction : Block) {
      const auto *Update = llvm::dyn_cast<llvm::BinaryOperator>(&Instruction);
      if (!Update || Analysis.Inlinable.count(Update) ||
          RawMemoryUpdateInputs.count(Update))
        continue;
      const auto *Load = RawLoad(*Update);
      unsigned Work = ::neverd::limits::backend::kLLVMCMemoryUpdateValueWork;
      if (!Load || !effectFreeBetween(*Load, *Update, Work))
        continue;
      RawMemoryExpressionLoads.insert(Load);
      Analysis.Inlinable.insert(Load);
      MaterializedExpressions.insert(Update);
    }
  }
  InlineCache.clear();
}

std::string LLVMCWriter::rawMemoryLoadText(const llvm::LoadInst &Load) {
  const auto Alias =
      c_memory::alias(typeToCLLVM(Load.getType()), Opts.ScalarPointers);
  return "(" + c_memory::access(Alias, valueStr(Load.getPointerOperand())) +
         ")";
}

bool LLVMCWriter::writeRawMemoryUpdate(const llvm::StoreInst &Store,
                                       int Indent) {
  auto Found = RawMemoryUpdates.find(&Store);
  if (Found == RawMemoryUpdates.end())
    return false;
  const auto *Update = Found->second;
  const auto *Constant =
      llvm::dyn_cast<llvm::ConstantInt>(Update->getOperand(1));
  if (!Constant)
    Constant = llvm::cast<llvm::ConstantInt>(Update->getOperand(0));
  const auto Alias =
      c_memory::alias(typeToCLLVM(Update->getType()), Opts.ScalarPointers);
  emitIndent(Indent);
  const bool Capture = !Update->hasOneUse();
  if (Capture)
    OS << getName(Update) << " = (";
  OS << c_memory::access(Alias, valueStr(Store.getPointerOperand())) << " "
     << updateOperator(Update->getOpcode()) << " " << valueStr(Constant);
  OS << (Capture ? ");\n" : ";\n");
  return true;
}

} // namespace neverd
