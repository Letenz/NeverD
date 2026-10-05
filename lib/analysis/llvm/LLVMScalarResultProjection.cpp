//===- LLVMScalarResultProjection.cpp - Retained scalar obligations
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/analysis/LLVMScalarResultProjection.h"

#include "neverd/analysis/LLVMScalarFunctionModel.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Errc.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>

namespace neverd::analysis {
namespace {
using namespace llvm;
using Result = LLVMScalarResultProjectionResult;

struct Failure {
  Result::Kind Status;
  std::string Message;
};

class Projector {
  const Function &Source;
  const LLVMScalarResultObservation &Observation;
  const LLVMScalarResultProjectionLimits &Limits;
  Result Output;
  SmallVector<const Metadata *, 16> MetadataWork;
  SmallVector<Type *, 16> TypeWork;
  SmallVector<const Function *, 8> Callees;

  void charge(uint64_t N = 1) {
    if (N > Limits.MaxConstructionWork - Output.ConstructionWork)
      throw Failure{Result::BudgetExceeded,
                    "scalar result projection work budget exhausted"};
    Output.ConstructionWork += N;
  }
  [[noreturn]] void fail(const char *Message) {
    throw Failure{Result::Unsupported, Message};
  }
  void attributes(AttributeList Attributes) {
    for (auto Set : Attributes)
      for (auto A : Set) {
        charge();
        if (A.isStringAttribute()) {
          charge(A.getKindAsString().size());
          charge(A.getValueAsString().size());
        }
        if (A.isConstantRangeListAttribute())
          charge(A.getValueAsConstantRangeList().size());
        if (A.isTypeAttribute())
          TypeWork.push_back(A.getValueAsType());
      }
  }
  template <typename T> void metadata(const T &Object) {
    SmallVector<std::pair<unsigned, MDNode *>, 4> Attachments;
    Object.getAllMetadata(Attachments);
    charge(Attachments.size());
    for (auto [Kind, Node] : Attachments) {
      (void)Kind;
      MetadataWork.push_back(Node);
    }
  }
  void constant(const Constant *Root) {
    SmallVector<const Constant *, 8> Work{Root};
    SmallPtrSet<const Constant *, 16> Seen;
    while (!Work.empty()) {
      const auto *C = Work.pop_back_val();
      charge();
      if (!Seen.insert(C).second)
        continue;
      if (isa<UndefValue, PoisonValue>(C))
        fail("undefined aggregate or scalar constant");
      if (isa<ConstantInt, ConstantAggregateZero, Function>(C))
        continue;
      if (!isa<ConstantStruct, ConstantArray>(C))
        fail("unsupported projection constant");
      charge(C->getNumOperands());
      for (const auto &Operand : C->operands())
        Work.push_back(cast<Constant>(Operand.get()));
    }
  }
  void preflight() {
    charge(Source.arg_size());
    charge(Source.size());
    charge(Observation.Indices.size());
    if (!Source.getParent() || Source.isDeclaration() || Source.isVarArg())
      fail("result projection requires a nonvariadic definition");
    if (Source.hasPersonalityFn() || Source.hasPrefixData() ||
        Source.hasPrologueData() || Source.hasGC() || Source.hasComdat())
      fail("unsupported projection execution contract");
    for (auto A : Source.getAttributes().getRetAttrs())
      if (!A.isEnumAttribute() || A.getKindAsEnum() != Attribute::NoUndef)
        fail("unsupported projection return contract");
    charge(Source.getParent()->getDataLayoutStr().size());
    charge(Source.getParent()->getTargetTriple().str().size());
    charge(Source.getName().size());
    attributes(Source.getAttributes());
    metadata(Source);
    TypeWork.push_back(Source.getFunctionType());
    for (const auto &A : Source.args())
      charge(A.getName().size());
    SmallPtrSet<const Function *, 8> SeenCallees;
    for (const auto &B : Source) {
      charge(B.size());
      charge(B.getName().size());
      for (const auto &I : B) {
        charge(I.getNumOperands());
        charge(I.getName().size());
        TypeWork.push_back(I.getType());
        metadata(I);
        if (auto *Call = dyn_cast<CallBase>(&I)) {
          auto *Callee = Call->getCalledFunction();
          if (!Callee || !Callee->isDeclaration() || !Callee->isIntrinsic() ||
              Callee->hasComdat())
            fail("only intrinsic declarations may be cloned");
          attributes(Call->getAttributes());
          if (SeenCallees.insert(Callee).second) {
            Callees.push_back(Callee);
            charge(Callee->getName().size());
            attributes(Callee->getAttributes());
            metadata(*Callee);
            TypeWork.push_back(Callee->getFunctionType());
          }
        }
        for (const auto *Operand : I.operand_values())
          if (auto *C = dyn_cast<Constant>(Operand))
            constant(C);
      }
    }
    SmallPtrSet<const Type *, 32> Types;
    while (!TypeWork.empty()) {
      auto *T = TypeWork.pop_back_val();
      charge();
      if (!Types.insert(T).second)
        continue;
      if (auto *A = dyn_cast<ArrayType>(T))
        charge(A->getNumElements());
      charge(T->getNumContainedTypes());
      for (auto *Sub : T->subtypes())
        TypeWork.push_back(Sub);
    }
    SmallPtrSet<const Metadata *, 32> Seen;
    while (!MetadataWork.empty()) {
      auto *M = MetadataWork.pop_back_val();
      charge();
      if (!M || !Seen.insert(M).second)
        continue;
      if (auto *S = dyn_cast<MDString>(M))
        charge(S->getString().size());
      else if (auto *N = dyn_cast<MDNode>(M)) {
        charge(N->getNumOperands());
        for (const auto &Operand : N->operands())
          MetadataWork.push_back(Operand.get());
      } else if (auto *V = dyn_cast<ValueAsMetadata>(M)) {
        if (!isa<ConstantInt>(V->getValue()))
          fail("unsupported metadata value in result projection");
      }
    }
    if (verifyFunction(Source))
      fail("invalid result projection input");
  }
  Type *leafType(Type *T, ArrayRef<unsigned> Indices) {
    for (auto Index : Indices) {
      charge();
      if (auto *S = dyn_cast<StructType>(T)) {
        if (Index >= S->getNumElements())
          fail("result field outside struct");
        T = S->getElementType(Index);
      } else if (auto *A = dyn_cast<ArrayType>(T)) {
        if (Index >= A->getNumElements())
          fail("result field outside array");
        T = A->getElementType();
      } else
        fail("result path does not designate an aggregate field");
    }
    return T;
  }
  const Value *leaf(const Value *V, ArrayRef<unsigned> Path) {
    while (!Path.empty()) {
      charge();
      if (auto *I = dyn_cast<InsertValueInst>(V)) {
        const auto Indices = I->getIndices();
        charge(Indices.size());
        if (Indices.size() <= Path.size() &&
            std::equal(Indices.begin(), Indices.end(), Path.begin())) {
          V = I->getInsertedValueOperand();
          Path = Path.drop_front(Indices.size());
        } else
          V = I->getAggregateOperand();
      } else if (auto *C = dyn_cast<Constant>(V)) {
        V = C->getAggregateElement(Path.front());
        if (!V)
          fail("unresolved constant result field");
        Path = Path.drop_front();
      } else
        fail("result leaf requires insertvalue or defined constant packaging");
    }
    return V;
  }
  void construct() {
    auto *Leaf = dyn_cast<IntegerType>(
        leafType(Source.getReturnType(), Observation.Indices));
    const auto Bits = Observation.Bits;
    if (!Leaf ||
        (Bits != 1 && Bits != 8 && Bits != 16 && Bits != 32 && Bits != 64) ||
        Leaf->getBitWidth() > 64 || Observation.LowBit >= Leaf->getBitWidth() ||
        Bits > Leaf->getBitWidth() - Observation.LowBit)
      fail("unsupported result bit window");
    SmallVector<std::pair<const ReturnInst *, const Value *>, 8> Returns;
    for (const auto &I : instructions(Source)) {
      charge();
      if (auto *R = dyn_cast<ReturnInst>(&I))
        Returns.push_back({R, leaf(R->getReturnValue(), Observation.Indices)});
      else if (isa<InsertValueInst>(I)) {
        SmallVector<std::pair<unsigned, MDNode *>, 4> Contracts;
        I.getAllMetadataOtherThanDebugLoc(Contracts);
        if (!Contracts.empty())
          fail("unsupported aggregate packaging metadata contract");
        charge(I.getNumUses());
        for (const auto *U : I.users())
          if (!isa<InsertValueInst, ReturnInst>(U))
            fail("aggregate packaging has a non-packaging use");
      } else
        ++Output.RetainedInstructions;
    }
    if (Returns.empty())
      fail("result projection requires a return observation");
    // Charge a second complete input traversal before LLVM copies the body,
    // attributes and metadata. LLVM's internal cloning remains trusted.
    charge(Output.ConstructionWork);
    auto M = std::make_unique<Module>("scalar.result.projection",
                                      Source.getContext());
    M->setDataLayout(Source.getParent()->getDataLayout());
    M->setTargetTriple(Source.getParent()->getTargetTriple());
    auto *ResultType = IntegerType::get(Source.getContext(), Bits);
    auto *F = Function::Create(
        FunctionType::get(ResultType, Source.getFunctionType()->params(),
                          false),
        Source.getLinkage(), Source.getName(), *M);
    F->copyAttributesFrom(&Source);
    ValueToValueMapTy Values;
    Values[&Source] = F;
    for (const auto &Arg : Source.args()) {
      auto *A = F->getArg(Arg.getArgNo());
      A->setName(Arg.getName());
      Values[&Arg] = A;
    }
    for (const auto *Callee : Callees) {
      auto *Copy =
          Function::Create(Callee->getFunctionType(), Callee->getLinkage(),
                           Callee->getName(), *M);
      Copy->copyAttributesFrom(Callee);
      Values[Callee] = Copy;
    }
    SmallVector<ReturnInst *, 8> ClonedReturns;
    CloneFunctionInto(F, &Source, Values,
                      CloneFunctionChangeType::DifferentModule, ClonedReturns);
    // LLVM may create an empty compile-unit list even without debug info.
    // It needs no module flags and must not make reparsing diagnose a bogus
    // debug version in an otherwise debug-free projection.
    if (auto *Units = M->getNamedMetadata("llvm.dbg.cu"))
      if (!Units->getNumOperands())
        Units->eraseFromParent();
    for (auto [Original, Selected] : Returns) {
      charge(3);
      auto *R = cast<ReturnInst>(Values.lookup(Original));
      Value *V = Values.lookup(Selected);
      if (!V)
        V = const_cast<Constant *>(dyn_cast<Constant>(Selected));
      if (!V)
        fail("unmapped scalar observation");
      IRBuilder<> B(R);
      if (Observation.LowBit)
        V = B.CreateLShr(V, Observation.LowBit);
      auto *Replacement = B.CreateRet(B.CreateTruncOrBitCast(V, ResultType));
      Replacement->copyMetadata(*R);
      Replacement->setDebugLoc(R->getDebugLoc());
      R->eraseFromParent();
    }
    SmallVector<InsertValueInst *, 16> Pending;
    SmallPtrSet<InsertValueInst *, 16> Queued;
    uint64_t Packaging = 0;
    for (auto &I : instructions(F)) {
      charge();
      if (auto *A = dyn_cast<InsertValueInst>(&I)) {
        ++Packaging;
        if (A->use_empty() && Queued.insert(A).second)
          Pending.push_back(A);
      }
    }
    while (!Pending.empty()) {
      charge();
      auto *I = Pending.pop_back_val();
      auto *Base = dyn_cast<InsertValueInst>(I->getAggregateOperand());
      auto *Inserted = dyn_cast<InsertValueInst>(I->getInsertedValueOperand());
      I->eraseFromParent();
      ++Output.RemovedPackaging;
      for (auto *Operand : {Base, Inserted})
        if (Operand && Operand->use_empty() && Queued.insert(Operand).second)
          Pending.push_back(Operand);
    }
    if (Output.RemovedPackaging != Packaging)
      fail("aggregate packaging remains observable");
    auto Model = modelLLVMScalarFunction(*F, Limits.Model);
    if (!Model) {
      handleAllErrors(Model.takeError(), [&](const ErrorInfoBase &E) {
        throw Failure{E.convertToErrorCode() == errc::result_out_of_range
                          ? Result::BudgetExceeded
                          : Result::Unsupported,
                      E.message()};
      });
    }
    Output.Status = Result::Projected;
    Output.Module = std::move(M);
  }

public:
  Projector(const Function &F, const LLVMScalarResultObservation &O,
            const LLVMScalarResultProjectionLimits &L)
      : Source(F), Observation(O), Limits(L) {}
  Result run() {
    try {
      preflight();
      construct();
    } catch (const Failure &E) {
      Output.Status = E.Status;
      Output.Diagnostic = E.Message;
    }
    return std::move(Output);
  }
};
} // namespace

LLVMScalarResultProjectionResult
projectLLVMScalarResult(const llvm::Function &Function,
                        const LLVMScalarResultObservation &Observation,
                        const LLVMScalarResultProjectionLimits &Limits) {
  return Projector(Function, Observation, Limits).run();
}
} // namespace neverd::analysis
