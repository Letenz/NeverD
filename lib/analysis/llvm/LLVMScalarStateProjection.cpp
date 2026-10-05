//===- LLVMScalarStateProjection.cpp - Declared state closure -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/analysis/LLVMScalarStateProjection.h"

#include "LLVMInterpreterModelInternal.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

namespace neverd::analysis {
namespace {
using namespace llvm;
using Result = LLVMScalarStateProjectionResult;
struct Failure {
  Result::Kind Status;
  std::string Message;
};

class Projector {
  const Function &Source;
  const LLVMScalarStateContract &Contract;
  const LLVMScalarStateProjectionLimits &Limits;
  Result Output;
  SmallVector<AllocaInst *, 32> Cells;
  SmallVector<Value *, 32> Initial;
  std::map<const Value *, int64_t> Pointers;
  SmallVector<const Function *, 8> Callees;

  void charge(uint64_t Count = 1) {
    if (Count > Limits.MaxConstructionWork - Output.ConstructionWork)
      throw Failure{Result::BudgetExceeded,
                    "scalar state construction budget exhausted"};
    Output.ConstructionWork += Count;
  }
  [[noreturn]] void fail(const char *Text) {
    throw Failure{Result::Unsupported, Text};
  }
  void bound(uint64_t Count, uint64_t Limit) {
    if (Count > Limit)
      throw Failure{Result::BudgetExceeded,
                    "scalar state storage limit exceeded"};
  }
  static bool wordBits(unsigned Bits) {
    return Bits == 8 || Bits == 16 || Bits == 32 || Bits == 64;
  }
  void preflight() {
    bound(Contract.StateBytes, Limits.MaxStateBytes);
    bound(Contract.Observations.size(), Limits.MaxObservations);
    charge(Contract.Entry.size());
    charge(Contract.Observations.size());
    charge(Contract.StateBytes);
    if (!Contract.StateBytes || !wordBits(Contract.CellBits) ||
        Contract.StateBytes % (Contract.CellBits / 8))
      fail("state extent must contain complete scalar cells");
    Output.Arguments = Contract.StateBytes / (Contract.CellBits / 8);
    bound(Output.Arguments, Limits.MaxArguments);
    std::set<unsigned> Seen;
    for (auto Mask : Contract.Entry)
      if (Mask.Cell >= Output.Arguments || !Seen.insert(Mask.Cell).second ||
          (Contract.CellBits < 64 &&
           ((Mask.And | Mask.Or) >> Contract.CellBits)))
        fail("invalid or repeated state entry mask");
    for (auto Observation : Contract.Observations)
      if (!wordBits(Observation.Bits) ||
          Observation.Offset >= Contract.StateBytes ||
          Observation.Bits / 8 > Contract.StateBytes - Observation.Offset)
        fail("state observation outside the declared object");
    if (Source.hasComdat())
      fail("module-owned COMDAT is unsupported in state projection");
    // This authoritative admission also rejects unproved integer-pointer
    // flags and state addresses used as ordinary scalar data.
    llvm_model::Builder Model(Source, Limits.Model, Contract.StateBytes);
    auto Graph = Model.build();
    charge(Model.statePointerOffsets().size());
    Pointers = Model.statePointerOffsets();
    SmallPtrSet<const Function *, 8> SeenCallees;
    charge(Source.getName().size());
    charge(Source.getParent()->getDataLayoutStr().size());
    charge(Source.getParent()->getTargetTriple().str().size());
    charge(Source.size());
    for (const auto &B : Source) {
      charge(B.size());
      charge(B.getName().size());
      for (const auto &I : B) {
        charge(I.getNumOperands());
        charge(I.getName().size());
        const Value *Pointer = nullptr;
        if (auto *L = dyn_cast<LoadInst>(&I)) {
          Pointer = L->getPointerOperand();
        }
        if (auto *S = dyn_cast<StoreInst>(&I)) {
          Pointer = S->getPointerOperand();
        }
        if (Pointer && !Pointers.count(Pointer))
          fail("external memory cannot be closed over scalar state");
        if (auto *Call = dyn_cast<CallBase>(&I)) {
          const auto *Callee = Call->getCalledFunction();
          if (!Callee || !Callee->isDeclaration() || !Callee->isIntrinsic() ||
              Callee->hasComdat())
            fail("only admitted intrinsic declarations may be cloned");
          if (SeenCallees.insert(Callee).second)
            Callees.push_back(Callee);
        }
      }
    }
  }

  Value *read(IRBuilder<> &B, unsigned Offset, unsigned Bits, bool Entry) {
    charge(6 * (Bits / 8));
    const auto CellBytes = Contract.CellBits / 8;
    auto *T = B.getIntNTy(Bits);
    Value *Result = nullptr;
    for (unsigned N = Offset; N < Offset + Bits / 8;) {
      const auto Index = N / CellBytes;
      const auto Within = N % CellBytes;
      const auto Take = std::min(CellBytes - Within, Offset + Bits / 8 - N);
      Value *Part =
          Entry ? Initial[Index]
                : B.CreateLoad(B.getIntNTy(Contract.CellBits), Cells[Index]);
      if (Within)
        Part = B.CreateLShr(Part, Within * 8);
      Part = B.CreateZExtOrTrunc(Part, T);
      if (Take * 8 < Bits)
        Part = B.CreateAnd(
            Part, ConstantInt::get(T, APInt::getLowBitsSet(Bits, Take * 8)));
      if (N != Offset)
        Part = B.CreateShl(Part, 8 * (N - Offset));
      Result = Result ? B.CreateOr(Result, Part) : Part;
      N += Take;
    }
    return Result;
  }
  void write(IRBuilder<> &B, unsigned Offset, Value *V) {
    const auto Bytes = V->getType()->getIntegerBitWidth() / 8;
    const auto CellBytes = Contract.CellBits / 8;
    auto *T = B.getIntNTy(Contract.CellBits);
    charge(8 * Bytes);
    for (unsigned N = Offset; N < Offset + Bytes;) {
      const auto Index = N / CellBytes;
      const auto Within = N % CellBytes;
      const auto Take = std::min(CellBytes - Within, Offset + Bytes - N);
      auto *Part = N == Offset ? V : B.CreateLShr(V, 8 * (N - Offset));
      Part = B.CreateZExtOrTrunc(Part, T);
      if (Within)
        Part = B.CreateShl(Part, Within * 8);
      if (Take != CellBytes) {
        auto Mask =
            APInt::getLowBitsSet(Contract.CellBits, Take * 8).shl(Within * 8);
        auto *Kept = B.CreateAnd(B.CreateLoad(T, Cells[Index]),
                                 ConstantInt::get(T, ~Mask));
        Part = B.CreateOr(Kept, B.CreateAnd(Part, ConstantInt::get(T, Mask)));
      }
      B.CreateStore(Part, Cells[Index]);
      N += Take;
    }
  }

  void statusObligation(IRBuilder<> &B, Value *Status) {
    const auto Attribute =
        Source.getAttributes().getRetAttrs().getAttribute(Attribute::Range);
    if (!Attribute.isValid())
      return;
    const auto Guard = llvm_model::rangeObligation(Attribute.getRange());
    if (Guard.Test == llvm_model::RangeObligation::None)
      return;
    charge(5);
    Value *Bad = B.getTrue();
    if (Guard.Test == llvm_model::RangeObligation::Bounds) {
      auto *Low = B.CreateICmpULT(Status, B.getInt64(Guard.Lower));
      auto *High = B.CreateICmpUGE(Status, B.getInt64(Guard.Upper));
      Bad = Guard.Wrapped ? B.CreateAnd(Low, High) : B.CreateOr(Low, High);
    }
    B.CreateAssumption(B.CreateNot(Bad));
  }
  void construct() {
    auto M = std::make_unique<Module>("scalar.state.projection",
                                      Source.getContext());
    M->setDataLayout(Source.getParent()->getDataLayout());
    M->setTargetTriple(Source.getParent()->getTargetTriple());
    auto &C = Source.getContext();
    auto *InputType = IntegerType::get(C, Contract.CellBits);
    charge(Output.Arguments);
    SmallVector<Type *, 32> Parameters(Output.Arguments, InputType);
    SmallVector<Type *, 16> Fields{Type::getInt64Ty(C)};
    for (auto Observation : Contract.Observations)
      Fields.push_back(IntegerType::get(C, Observation.Bits));
    auto *ReturnType = StructType::get(C, Fields);
    auto *F = Function::Create(FunctionType::get(ReturnType, Parameters, false),
                               Source.getLinkage(), Source.getName(), *M);
    F->copyAttributesFrom(&Source);
    SmallVector<AttributeSet, 32> ArgumentAttributes(
        Output.Arguments,
        AttributeSet::get(C, {Attribute::get(C, Attribute::NoUndef)}));
    F->setAttributes(AttributeList::get(C, Source.getAttributes().getFnAttrs(),
                                        {}, ArgumentAttributes));
    auto *Entry = BasicBlock::Create(C, "state.entry", F);
    IRBuilder<> B(Entry);
    charge(Output.Arguments);
    for (unsigned N = 0; N < Output.Arguments; ++N)
      Cells.push_back(B.CreateAlloca(InputType));
    SmallVector<Value *, 32> Inputs;
    for (auto &A : F->args()) {
      A.setName("cell" + Twine(A.getArgNo()));
      Inputs.push_back(&A);
    }
    for (auto Mask : Contract.Entry) {
      charge(2);
      Inputs[Mask.Cell] = B.CreateOr(
          B.CreateAnd(Inputs[Mask.Cell], ConstantInt::get(InputType, Mask.And)),
          ConstantInt::get(InputType, Mask.Or));
    }
    charge(Output.Arguments);
    Initial = Inputs;
    for (unsigned N = 0; N < Output.Arguments; ++N)
      B.CreateStore(Initial[N], Cells[N]);
    ValueToValueMapTy Values;
    Values[&Source] = F;
    // This placeholder never escapes: all admitted pointer projections and
    // accesses are removed before verification or promotion.
    Values[Source.getArg(0)] = Cells.front();
    for (const auto *Callee : Callees) {
      auto *Copy =
          Function::Create(Callee->getFunctionType(), Callee->getLinkage(),
                           Callee->getName(), *M);
      Copy->copyAttributesFrom(Callee);
      Values[Callee] = Copy;
    }
    SmallVector<ReturnInst *, 8> Returns;
    CloneFunctionInto(F, &Source, Values,
                      CloneFunctionChangeType::DifferentModule, Returns);
    // CloneFunctionInto copies attributes too; restore the new scalar contract.
    F->setAttributes(AttributeList::get(C, Source.getAttributes().getFnAttrs(),
                                        {}, ArgumentAttributes));
    B.CreateBr(cast<BasicBlock>(Values.lookup(&Source.getEntryBlock())));
    if (auto *Units = M->getNamedMetadata("llvm.dbg.cu"))
      if (!Units->getNumOperands())
        Units->eraseFromParent();
    for (const auto &I : instructions(Source)) {
      charge();
      auto *Clone = cast<Instruction>(Values.lookup(&I));
      IRBuilder<> At(Clone);
      if (auto *L = dyn_cast<LoadInst>(&I)) {
        auto *V = read(At, Pointers.at(L->getPointerOperand()),
                       L->getType()->getIntegerBitWidth(), false);
        Clone->replaceAllUsesWith(V);
        Clone->eraseFromParent();
        ++Output.Loads;
      } else if (auto *S = dyn_cast<StoreInst>(&I)) {
        write(At, Pointers.at(S->getPointerOperand()), Clone->getOperand(0));
        Clone->eraseFromParent();
        ++Output.Stores;
      } else if (isa<ReturnInst>(I)) {
        auto *Status = Clone->getOperand(0);
        statusObligation(At, Status);
        Value *Aggregate = At.CreateInsertValue(
            Constant::getNullValue(ReturnType), Status, {0});
        unsigned Index = 1;
        for (auto O : Contract.Observations) {
          auto *V = read(At, O.Offset, O.Bits, false);
          if (O.DifferenceFromEntry) {
            charge();
            V = At.CreateXor(V, read(At, O.Offset, O.Bits, true));
          }
          charge();
          Aggregate = At.CreateInsertValue(Aggregate, V, {Index++});
        }
        charge(2);
        auto *Return = At.CreateRet(Aggregate);
        Return->copyMetadata(*Clone);
        Clone->eraseFromParent();
      }
    }
    SmallVector<Instruction *, 16> Projections;
    SmallPtrSet<Instruction *, 16> Projected;
    for (auto [Original, Offset] : Pointers) {
      (void)Offset;
      charge();
      if (isa<Instruction>(Original)) {
        auto *I = cast<Instruction>(Values.lookup(Original));
        Projections.push_back(I);
        Projected.insert(I);
      }
    }
    for (auto *I : Projections) {
      charge(I->getNumUses());
      for (auto *U : I->users())
        if (!Projected.contains(dyn_cast<Instruction>(U)))
          fail("state address remains observable");
    }
    for (auto *I : Projections)
      I->dropAllReferences();
    for (auto *I : Projections)
      I->eraseFromParent();
    // Account for the dense block/cell upper bound before LLVM places PHIs.
    charge(uint64_t(Cells.size()) * F->size());
    DominatorTree Dominators(*F);
    PromoteMemToReg(Cells, Dominators);
    if (verifyModule(*M))
      fail("invalid scalar state projection");
    LLVMScalarResultProjectionLimits Validation;
    Validation.MaxConstructionWork =
        Limits.MaxConstructionWork - Output.ConstructionWork;
    Validation.Model = Limits.Model;
    auto Scalar = projectLLVMScalarResult(*F, {{0}, 0, 64}, Validation);
    charge(Scalar.ConstructionWork);
    if (!Scalar.Module)
      throw Failure{Scalar.Status ==
                            LLVMScalarResultProjectionResult::BudgetExceeded
                        ? Result::BudgetExceeded
                        : Result::Unsupported,
                    Scalar.Diagnostic};
    Output.Status = Result::Projected;
    Output.Module = std::move(M);
  }

public:
  Projector(const Function &F, const LLVMScalarStateContract &C,
            const LLVMScalarStateProjectionLimits &L)
      : Source(F), Contract(C), Limits(L) {}
  Result run() {
    try {
      preflight();
      construct();
    } catch (const Failure &E) {
      Output.Status = E.Status;
      Output.Diagnostic = E.Message;
    } catch (const llvm_model::Failure &E) {
      Output.Status = E.Budget ? Result::BudgetExceeded : Result::Unsupported;
      Output.Diagnostic = E.Message;
    }
    return std::move(Output);
  }
};
} // namespace

LLVMScalarStateProjectionResult
projectLLVMScalarState(const llvm::Function &Function,
                       const LLVMScalarStateContract &Contract,
                       const LLVMScalarStateProjectionLimits &Limits) {
  return Projector(Function, Contract, Limits).run();
}
} // namespace neverd::analysis
