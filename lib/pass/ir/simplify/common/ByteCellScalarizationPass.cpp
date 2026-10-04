//===- ByteCellScalarizationPass.cpp - Promote overlapping cells ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/pass/ir/simplify/ByteCellScalarizationPass.h"

#include "neverd/analysis/LLVMMemoryAnalysis.h"
#include "neverd/pass/ir/simplify/SymSimplifyPass.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>

namespace neverd {
namespace {
using namespace llvm;

struct Access {
  Instruction *Operation;
  uint64_t Offset;
  unsigned Bytes;
  unsigned First = 0;
  unsigned Count = 0;
};

struct Cell {
  uint64_t Offset;
  unsigned Bytes;
};

struct Object {
  AllocaInst *Allocation;
  SmallVector<Access> Accesses;
  SmallVector<Cell> Cells;
};

class Scalarization {
  Function &F;
  const DataLayout &DL;
  const ByteCellScalarizationOptions &Options;
  ByteCellScalarizationResult Result;
  SmallVector<Object> Objects;
  uint64_t PlannedCells = 0;
  uint64_t PlannedInstructions = 0;

  bool exhaust() {
    Result.BudgetExhausted = true;
    return false;
  }

  bool charge(uint64_t Amount = 1) {
    if (Amount > Options.MaxWork - Result.Work)
      return exhaust();
    Result.Work += Amount;
    return true;
  }

  bool collect(Object &O, uint64_t Size) {
    SmallVector<std::pair<Value *, uint64_t>> Work{{O.Allocation, 0}};
    SmallPtrSet<Value *, 32> Seen;
    const unsigned Width = DL.getIndexSizeInBits(0);
    while (!Work.empty()) {
      auto [Pointer, Offset] = Work.pop_back_val();
      if (!charge())
        return false;
      if (!Seen.insert(Pointer).second)
        continue;
      if (Pointer->isUsedByMetadata())
        return false;
      for (auto *User : Pointer->users()) {
        if (!charge())
          return false;
        auto *I = dyn_cast<Instruction>(User);
        if (!I || I->hasMetadataOtherThanDebugLoc() || I->hasDbgRecords())
          return false;
        if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) {
          if (!charge(GEP->getNumIndices()))
            return false;
          APInt Delta(Width, 0);
          if (GEP->getPointerOperand() != Pointer ||
              !GEP->accumulateConstantOffset(DL, Delta))
            return false;
          bool Overflow = false;
          APInt Sum = APInt(Width, Offset).sadd_ov(Delta, Overflow);
          if (Overflow || Sum.isNegative() || Sum.getZExtValue() > Size)
            return false;
          Work.emplace_back(GEP, Sum.getZExtValue());
          continue;
        }
        auto *Load = dyn_cast<LoadInst>(I);
        auto *Store = dyn_cast<StoreInst>(I);
        Type *Type = nullptr;
        if (Load && Load->isSimple() && Load->getPointerOperand() == Pointer)
          Type = Load->getType();
        if (Store && Store->isSimple() && Store->getPointerOperand() == Pointer)
          Type = Store->getValueOperand()->getType();
        if (!Type || !Type->isIntegerTy() || Type->getIntegerBitWidth() % 8 ||
            Type->getIntegerBitWidth() > 128)
          return false;
        const unsigned Bytes = Type->getIntegerBitWidth() / 8;
        if (Offset > Size || Bytes > Size - Offset)
          return false;
        O.Accesses.push_back({I, Offset, Bytes});
      }
    }
    return true;
  }

  bool partition(Object &O) {
    if (O.Accesses.empty())
      return false;
    SmallVector<std::pair<uint64_t, int>> Events;
    if (!charge(2 * O.Accesses.size()))
      return false;
    for (const auto &A : O.Accesses) {
      Events.emplace_back(A.Offset, 1);
      Events.emplace_back(A.Offset + A.Bytes, -1);
    }
    // Charge a sorting upper bound before sorting, then account for each
    // event, cell and access lookup separately. No byte-by-access cross
    // product.
    if (!charge(Events.size() * (1 + Log2_64_Ceil(Events.size()))))
      return false;
    llvm::sort(Events);
    int64_t Active = 0;
    bool ProperOverlap = false;
    for (size_t I = 0; I < Events.size();) {
      uint64_t At = Events[I].first;
      do {
        if (!charge())
          return false;
        Active += Events[I++].second;
      } while (I < Events.size() && Events[I].first == At);
      if (I == Events.size() || !Active)
        continue;
      const uint64_t End = Events[I].first;
      while (At < End) {
        if (!charge())
          return false;
        if (PlannedCells + O.Cells.size() >= Options.MaxCells)
          return exhaust();
        unsigned Bytes = 8;
        while (Bytes > End - At)
          Bytes /= 2;
        O.Cells.push_back({At, Bytes});
        At += Bytes;
      }
    }
    uint64_t NewInstructions = O.Cells.size();
    for (auto &A : O.Accesses) {
      if (!charge(1 + Log2_64_Ceil(O.Cells.size())))
        return false;
      auto First = llvm::lower_bound(
          O.Cells, A.Offset,
          [](const Cell &C, uint64_t Offset) { return C.Offset < Offset; });
      A.First = First - O.Cells.begin();
      for (auto It = First;
           It != O.Cells.end() && It->Offset < A.Offset + A.Bytes; ++It) {
        if (!charge())
          return false;
        ++A.Count;
      }
      // A proper boundary comes from an actual access endpoint, not the
      // 64-bit cell ceiling alone. This keeps exact wide homes in SROA.
      if (!charge(1 + Log2_64_Ceil(Events.size())))
        return false;
      auto Boundary =
          llvm::upper_bound(Events, std::pair<uint64_t, int>{A.Offset, 1});
      ProperOverlap |=
          Boundary != Events.end() && Boundary->first < A.Offset + A.Bytes;
      NewInstructions += A.Count == 1 ? 1 : 2 + 3 * A.Count;
    }
    if (!ProperOverlap)
      return false;
    if (NewInstructions > Options.MaxNewInstructions - PlannedInstructions ||
        !charge(NewInstructions))
      return exhaust();
    PlannedCells += O.Cells.size();
    PlannedInstructions += NewInstructions;
    return true;
  }

  void rewrite(Object &O) {
    IRBuilder<> Entry(&*F.getEntryBlock().getFirstInsertionPt());
    SmallVector<AllocaInst *> Homes;
    for (const auto &C : O.Cells)
      Homes.push_back(Entry.CreateAlloca(Entry.getIntNTy(C.Bytes * 8), nullptr,
                                         "memory.cell"));
    for (const auto &A : O.Accesses) {
      IRBuilder<> B(A.Operation);
      B.SetCurrentDebugLocation(A.Operation->getDebugLoc());
      auto *Store = dyn_cast<StoreInst>(A.Operation);
      if (A.Count == 1) {
        if (Store)
          B.CreateStore(Store->getValueOperand(), Homes[A.First]);
        else
          A.Operation->replaceAllUsesWith(
              B.CreateLoad(A.Operation->getType(), Homes[A.First]));
      } else {
        // Keep the original stored value as ONE memory operand. Extracting
        // several arithmetic fragments instead could duplicate an undef
        // choice. Typed private memory also preserves partial poison and the
        // target byte order, without inserting freeze or assuming noundef.
        auto *Copy = Entry.CreateAlloca(
            ArrayType::get(Entry.getInt8Ty(), A.Bytes), nullptr, "access.copy");
        if (Store)
          B.CreateAlignedStore(Store->getValueOperand(), Copy, Align(1));
        for (unsigned I = A.First; I < A.First + A.Count; ++I) {
          const auto &C = O.Cells[I];
          auto *At = B.CreateGEP(
              B.getInt8Ty(), Copy,
              B.getIntN(DL.getIndexSizeInBits(0), C.Offset - A.Offset));
          auto *Type = B.getIntNTy(C.Bytes * 8);
          if (Store)
            B.CreateStore(B.CreateAlignedLoad(Type, At, Align(1)), Homes[I]);
          else
            B.CreateAlignedStore(B.CreateLoad(Type, Homes[I]), At, Align(1));
        }
        if (!Store)
          A.Operation->replaceAllUsesWith(
              B.CreateAlignedLoad(A.Operation->getType(), Copy, Align(1)));
      }
      A.Operation->eraseFromParent();
    }
    ++Result.Objects;
    Result.Cells += O.Cells.size();
    Result.Accesses += O.Accesses.size();
  }

public:
  Scalarization(Function &F, const ByteCellScalarizationOptions &Options)
      : F(F), DL(F.getParent()->getDataLayout()), Options(Options) {}

  ByteCellScalarizationResult run() {
    if (DL.getAllocaAddrSpace() != 0 || DL.isNonIntegralAddressSpace(0) ||
        (DL.getPointerSizeInBits(0) != 32 &&
         DL.getPointerSizeInBits(0) != 64) ||
        DL.getIndexSizeInBits(0) != DL.getPointerSizeInBits(0))
      return Result;
    SmallVector<AllocaInst *> Allocations;
    for (auto &I : instructions(F)) {
      if (Result.Instructions == Options.MaxInstructions || !charge()) {
        exhaust();
        return Result;
      }
      ++Result.Instructions;
      if (I.hasDbgRecords() ||
          (isa<CallBase>(I) && !analysis::isLLVMFrameIndependentIntrinsic(I)))
        return Result;
      if (auto *A = dyn_cast<AllocaInst>(&I)) {
        auto *Type = dyn_cast<ArrayType>(A->getAllocatedType());
        auto *Count = dyn_cast<ConstantInt>(A->getArraySize());
        if (A->isStaticAlloca() && !A->isUsedWithInAlloca() &&
            !A->isSwiftError() && A->getAddressSpace() == 0 && Count &&
            Count->isOne() && Type && Type->getElementType()->isIntegerTy(8) &&
            Type->getNumElements() &&
            Type->getNumElements() <= Options.MaxObjectBytes &&
            Type->getNumElements() < (uint64_t(1) << 31) &&
            !A->hasMetadataOtherThanDebugLoc())
          Allocations.push_back(A);
      }
    }
    for (auto *A : Allocations) {
      Object O{A, {}, {}};
      const auto Size =
          cast<ArrayType>(A->getAllocatedType())->getNumElements();
      if (collect(O, Size) && partition(O))
        Objects.push_back(std::move(O));
      if (Result.BudgetExhausted)
        return Result;
    }
    // All validation, storage ceilings and construction charges have succeeded
    // before the first mutation. Refused objects keep their complete use graph.
    for (auto &O : Objects)
      rewrite(O);
    Result.NewInstructions = PlannedInstructions;
    return Result;
  }
};
} // namespace

ByteCellScalarizationResult
ByteCellScalarizationPass::scalarize(llvm::Function &F,
                                     ByteCellScalarizationOptions Options) {
  if (F.isDeclaration() || !F.getParent() ||
      F.hasFnAttribute(kObfuscatedFnAttr) ||
      F.hasFnAttribute(llvm::Attribute::Naked))
    return {};
  return Scalarization(F, Options).run();
}

llvm::PreservedAnalyses
ByteCellScalarizationPass::run(llvm::Function &F,
                               llvm::FunctionAnalysisManager &) {
  return scalarize(F, Options).Objects ? llvm::PreservedAnalyses::none()
                                       : llvm::PreservedAnalyses::all();
}
} // namespace neverd
