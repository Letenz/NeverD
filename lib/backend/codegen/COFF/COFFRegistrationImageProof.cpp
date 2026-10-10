//===- COFFRegistrationImageProof.cpp - PE32 symbolic image pointers ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationIRProof.h"

#include "neverd/Limits.h"

#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Operator.h"

#include <set>

#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
namespace neverd::coff_registration {

llvm::Error validateSymbolicImagePointers(const llvm::Module &Module,
                                          uint64_t ImageBase,
                                          uint64_t ImageSize) {
  std::vector<const llvm::Value *> Pending, PointerIntegers;
  std::map<const llvm::Value *, std::vector<const llvm::Value *>> StoredValues;
  for (const auto &Global : Module.globals())
    if (Global.hasInitializer())
      Pending.push_back(Global.getInitializer());
  for (const auto &Function : Module)
    for (const auto &Block : Function)
      for (const auto &I : Block) {
        Pending.push_back(&I);
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I))
          StoredValues[llvm::getUnderlyingObject(Store->getPointerOperand())]
              .push_back(Store->getValueOperand());
      }
  std::set<const llvm::Value *> Seen;
  size_t Work = 0;
  while (!Pending.empty()) {
    const auto *Value = Pending.back();
    Pending.pop_back();
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return rejectIR("symbolic pointer audit exceeds the work budget");
    if (!Seen.insert(Value).second || llvm::isa<llvm::GlobalValue>(Value))
      continue;
    const auto *User = llvm::dyn_cast<llvm::User>(Value);
    if (!User)
      continue;
    unsigned Opcode = 0;
    if (const auto *I = llvm::dyn_cast<llvm::Instruction>(Value))
      Opcode = I->getOpcode();
    if (const auto *Expression = llvm::dyn_cast<llvm::ConstantExpr>(Value))
      Opcode = Expression->getOpcode();
    if (Opcode == llvm::Instruction::IntToPtr)
      PointerIntegers.push_back(User->getOperand(0));
    for (const auto &Operand : User->operands())
      Pending.push_back(Operand.get());
  }
  // A private scalar spill does not turn an absolute image address into a
  // relocatable value. Audit the integer def-use graph and every possible
  // store to a loaded local/global object, rather than only direct constants.
  using Constants = std::set<llvm::ConstantInt *>;
  std::map<const llvm::Value *, Constants> NumericValues;
  std::set<const llvm::Value *> Evaluating;
  bool NumericBudgetExceeded = false;
  bool NumericChanged = false;
  bool IncompleteNumericMemory = false;
  std::function<Constants(const llvm::Value *, unsigned)> Evaluate;
  Evaluate = [&](const llvm::Value *Value, unsigned Depth) -> Constants {
    if (NumericBudgetExceeded || ++Work > limits::kMaxRegistrationEHStateWork ||
        Depth > 128) {
      NumericBudgetExceeded = true;
      return {};
    }
    if (!Value->getType()->isIntegerTy())
      return {};
    if (!Evaluating.insert(Value).second)
      return NumericValues[Value];
    Constants Values;
    auto Add = [&](const Constants &Other) {
      Values.insert(Other.begin(), Other.end());
      if (Values.size() > 32)
        NumericBudgetExceeded = true;
    };
    if (const auto *C = llvm::dyn_cast<llvm::ConstantInt>(Value)) {
      Values.insert(const_cast<llvm::ConstantInt *>(C));
    } else if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Value)) {
      llvm::SmallVector<const llvm::Value *, 8> Objects;
      llvm::getUnderlyingObjects(Load->getPointerOperand(), Objects);
      for (const auto *Object : Objects) {
        if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Object);
            Global && Global->hasInitializer()) {
          if (Global->getInitializer()->getType() != Value->getType())
            IncompleteNumericMemory = true;
          else
            Add(Evaluate(Global->getInitializer(), Depth + 1));
        }
        if (auto Stores = StoredValues.find(Object);
            Stores != StoredValues.end()) {
          for (const auto *Stored : Stores->second) {
            if (Stored->getType() != Value->getType())
              IncompleteNumericMemory = true;
            else
              Add(Evaluate(Stored, Depth + 1));
          }
        }
      }
    } else if (const auto *Phi = llvm::dyn_cast<llvm::PHINode>(Value)) {
      for (const auto &Incoming : Phi->incoming_values())
        Add(Evaluate(Incoming.get(), Depth + 1));
    } else if (const auto *Select = llvm::dyn_cast<llvm::SelectInst>(Value)) {
      Add(Evaluate(Select->getTrueValue(), Depth + 1));
      Add(Evaluate(Select->getFalseValue(), Depth + 1));
    } else if (const auto *Argument = llvm::dyn_cast<llvm::Argument>(Value)) {
      for (const auto *User : Argument->getParent()->users()) {
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(User);
            Call &&
            Call->getCalledOperand()->stripPointerCasts() ==
                Argument->getParent() &&
            Argument->getArgNo() < Call->arg_size())
          Add(Evaluate(Call->getArgOperand(Argument->getArgNo()), Depth + 1));
        else if (Argument->getParent()->hasLocalLinkage())
          IncompleteNumericMemory = true;
      }
    } else if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(Value)) {
      llvm::SmallVector<const llvm::Value *, 8> Targets;
      llvm::getUnderlyingObjects(Call->getCalledOperand(), Targets);
      for (const auto *Target : Targets) {
        const auto *Callee = llvm::dyn_cast<llvm::Function>(Target);
        if (!Callee) {
          IncompleteNumericMemory = true;
          continue;
        }
        if (!Callee->isDeclaration())
          for (const auto &Block : *Callee)
            if (const auto *Return =
                    llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator());
                Return && Return->getReturnValue())
              Add(Evaluate(Return->getReturnValue(), Depth + 1));
      }
    } else if (const auto *Binary =
                   llvm::dyn_cast<llvm::BinaryOperator>(Value)) {
      auto Left = Evaluate(Binary->getOperand(0), Depth + 1);
      auto Right = Evaluate(Binary->getOperand(1), Depth + 1);
      for (auto *L : Left)
        for (auto *R : Right) {
          if (++Work > limits::kMaxRegistrationEHStateWork) {
            NumericBudgetExceeded = true;
            break;
          }
          if (auto *C = llvm::dyn_cast_or_null<llvm::ConstantInt>(
                  llvm::ConstantFoldBinaryOpOperands(Binary->getOpcode(), L, R,
                                                     Module.getDataLayout())))
            Add({C});
        }
    } else if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Value);
               Cast && Cast->getSrcTy()->isIntegerTy()) {
      for (auto *Operand : Evaluate(Cast->getOperand(0), Depth + 1))
        if (auto *C = llvm::dyn_cast_or_null<llvm::ConstantInt>(
                llvm::ConstantFoldCastOperand(Cast->getOpcode(), Operand,
                                              Cast->getDestTy(),
                                              Module.getDataLayout())))
          Add({C});
    }
    Evaluating.erase(Value);
    auto &Known = NumericValues[Value];
    const size_t PreviousSize = Known.size();
    Known.insert(Values.begin(), Values.end());
    NumericChanged |= Known.size() != PreviousSize;
    NumericBudgetExceeded |= Known.size() > 32;
    return Known;
  };
  Seen.clear();
  while (!PointerIntegers.empty()) {
    const auto *Value = PointerIntegers.back();
    PointerIntegers.pop_back();
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return rejectIR("symbolic pointer origin audit exceeds the work budget");
    if (!Seen.insert(Value).second)
      continue;
    // Cyclic PHIs and memory spills need a monotone closure. An empty result
    // during recursive evaluation is not evidence that the cycle is symbolic.
    do {
      NumericChanged = false;
      for (const auto *Numeric : Evaluate(Value, 0)) {
        const uint64_t Address =
            Numeric->getValue().zextOrTrunc(32).getZExtValue();
        if (Address >= ImageBase && Address - ImageBase < ImageSize)
          return rejectIR(
              "raw image pointer has no symbolic HIGHLOW relocation owner");
      }
    } while (NumericChanged && !NumericBudgetExceeded);
    if (IncompleteNumericMemory)
      return rejectIR(
          "raw image pointer origin has an unproved partial memory definition");
    if (NumericBudgetExceeded)
      return rejectIR(
          "symbolic pointer constant audit exceeds the work budget");
    if (llvm::isa<llvm::ConstantInt>(Value)) {
      continue;
    }
    if (const auto *I = llvm::dyn_cast<llvm::Instruction>(Value))
      if (const auto *Folded =
              llvm::ConstantFoldInstruction(I, Module.getDataLayout());
          Folded && Folded != Value)
        PointerIntegers.push_back(Folded);
    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Value)) {
      llvm::SmallVector<const llvm::Value *, 8> Objects;
      llvm::getUnderlyingObjects(Load->getPointerOperand(), Objects);
      for (const auto *Object : Objects) {
        if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Object);
            Global && Global->hasInitializer())
          PointerIntegers.push_back(Global->getInitializer());
        if (auto Stores = StoredValues.find(Object);
            Stores != StoredValues.end())
          PointerIntegers.insert(PointerIntegers.end(), Stores->second.begin(),
                                 Stores->second.end());
      }
    }
    // Calls produce runtime values. Symbolic globals already carry MC fixups;
    // neither is a numeric address leaf in the generated program.
    if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Value);
        Global && Global->hasInitializer())
      PointerIntegers.push_back(Global->getInitializer());
    if (llvm::isa<llvm::GlobalValue>(Value) || llvm::isa<llvm::CallBase>(Value))
      continue;
    if (const auto *User = llvm::dyn_cast<llvm::User>(Value))
      for (const auto &Operand : User->operands())
        PointerIntegers.push_back(Operand.get());
  }
  return llvm::Error::success();
}
} // namespace neverd::coff_registration
#endif
