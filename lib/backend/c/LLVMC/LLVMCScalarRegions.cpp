//===- LLVMCScalarRegions.cpp - Bounded scalar CFG structuring ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LLVMCWriter.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"

#include <functional>

namespace neverd {
namespace {

// Keep source structuring separate from semantic recovery. Every admitted
// block and successor edge is emitted once; the existing instruction and
// parallel-PHI writers retain their semantics. No IR rewrite is involved.
struct ScalarRegion {
  enum class Kind { Block, Edge, If, Loop } K;
  llvm::BasicBlock *Block = nullptr;
  llvm::BasicBlock *Target = nullptr;
  bool Invert = false;
  std::vector<ScalarRegion> First;
  std::vector<ScalarRegion> Second;
  llvm::BasicBlock *EntryPred = nullptr;
  llvm::BasicBlock *Latch = nullptr;
  const llvm::PHINode *Counter = nullptr;
};

class ScalarRegionPlan {
  llvm::Function &Fn;
  llvm::LoopInfo Loops;
  llvm::PostDominatorTree PostDominators;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 32> Seen;
  unsigned Edges = 0;
  unsigned Work = 0;

  bool edge(std::vector<ScalarRegion> &Into, llvm::BasicBlock *From,
            llvm::BasicBlock *To) {
    if (++Work > 65536)
      return false;
    Into.push_back({ScalarRegion::Kind::Edge, From, To, false, {}, {}});
    ++Edges;
    return true;
  }

  bool sequence(llvm::BasicBlock *BB, llvm::BasicBlock *Stop,
                const llvm::Loop *Scope, std::vector<ScalarRegion> &Into,
                unsigned Depth) {
    if (Depth > 64)
      return false;
    while (BB && BB != Stop) {
      if (++Work > 65536 || (Scope && !Scope->contains(BB)) ||
          !Seen.insert(BB).second)
        return false;
      const llvm::Loop *Loop = Loops.getLoopFor(BB);
      if (Loop && Loop->getHeader() == BB && Loop != Scope) {
        auto *Branch = llvm::dyn_cast<llvm::CondBrInst>(BB->getTerminator());
        // A header exit gives a direct while loop. Other exits/latches keep
        // the existing goto projection until a complete region is available.
        if (!Branch || Loop->getExitingBlock() != BB || !Loop->getLoopLatch() ||
            Loop->getParentLoop() != Scope)
          return false;
        const bool TrueBody = Loop->contains(Branch->getSuccessor(0));
        auto *Body = Branch->getSuccessor(TrueBody ? 0 : 1);
        auto *Exit = Branch->getSuccessor(TrueBody ? 1 : 0);
        if (!Loop->contains(Body) || Loop->contains(Exit))
          return false;
        ScalarRegion Region{
            ScalarRegion::Kind::Loop, BB, Exit, !TrueBody, {}, {}};
        Region.EntryPred = Loop->getLoopPredecessor();
        Region.Latch = Loop->getLoopLatch();
        if (!edge(Region.First, BB, Body) ||
            !sequence(Body, BB, Loop, Region.First, Depth + 1) ||
            !edge(Region.Second, BB, Exit))
          return false;
        Into.push_back(std::move(Region));
        BB = Exit;
        continue;
      }
      if (Loop != Scope)
        return false;
      Into.push_back({ScalarRegion::Kind::Block, BB, nullptr, false, {}, {}});
      auto *Term = BB->getTerminator();
      if (llvm::isa<llvm::ReturnInst>(Term))
        return Stop == nullptr;
      if (auto *Branch = llvm::dyn_cast<llvm::UncondBrInst>(Term)) {
        auto *Next = Branch->getSuccessor(0);
        if (!edge(Into, BB, Next))
          return false;
        BB = Next;
        continue;
      }
      auto *Branch = llvm::dyn_cast<llvm::CondBrInst>(Term);
      if (!Branch || Branch->getSuccessor(0) == Branch->getSuccessor(1))
        return false;
      auto *Node = PostDominators.getNode(BB);
      auto *Join =
          Node && Node->getIDom() ? Node->getIDom()->getBlock() : nullptr;
      if ((Scope && (!Join || !Scope->contains(Join))) || Join == BB)
        return false;
      ScalarRegion Region{ScalarRegion::Kind::If, BB, Join, false, {}, {}};
      for (unsigned I = 0; I != 2; ++I) {
        auto &Arm = I == 0 ? Region.First : Region.Second;
        if (!edge(Arm, BB, Branch->getSuccessor(I)) ||
            !sequence(Branch->getSuccessor(I), Join, Scope, Arm, Depth + 1))
          return false;
      }
      Into.push_back(std::move(Region));
      BB = Join;
    }
    return BB == Stop;
  }

public:
  explicit ScalarRegionPlan(llvm::Function &Fn) : Fn(Fn) {}

  bool build(std::vector<ScalarRegion> &Regions, llvm::DominatorTree &DT) {
    // Bound analysis construction too, and leave memory, EH and aggregate
    // source ownership to their existing projections. Integer observer calls
    // remain admissible and are emitted at their original instruction site.
    if (Fn.size() > 1024 || Fn.hasPersonalityFn())
      return false;
    auto Scalar = [](llvm::Type *T) {
      return T->isIntegerTy(1) || T->isIntegerTy(8) || T->isIntegerTy(16) ||
             T->isIntegerTy(32) || T->isIntegerTy(64);
    };
    if (!Fn.getReturnType()->isVoidTy() && !Scalar(Fn.getReturnType()))
      return false;
    for (const auto &Arg : Fn.args())
      if (!Scalar(Arg.getType()))
        return false;
    unsigned Instructions = 0, ExpectedEdges = 0;
    for (auto &BB : Fn) {
      if (BB.hasAddressTaken() || !DT.isReachableFromEntry(&BB))
        return false;
      auto *Term = BB.getTerminator();
      if (!llvm::isa<llvm::UncondBrInst, llvm::CondBrInst, llvm::ReturnInst>(
              Term))
        return false;
      ExpectedEdges += Term->getNumSuccessors();
      for (const auto &Inst : BB) {
        if (++Instructions > 16384)
          return false;
        if (Inst.isTerminator())
          continue;
        if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst)) {
          if (!Call->getCalledFunction() || Call->isConvergent() ||
              Call->isMustTailCall() || Call->hasOperandBundles())
            return false;
          for (const auto &Arg : Call->args())
            if (!Scalar(Arg->getType()))
              return false;
        } else {
          if (Inst.mayReadOrWriteMemory() || Inst.mayHaveSideEffects())
            return false;
          for (const auto &Op : Inst.operands())
            if (!Scalar(Op->getType()))
              return false;
        }
        if (!Inst.getType()->isVoidTy() && !Scalar(Inst.getType()))
          return false;
      }
    }
    Loops.analyze(DT);
    if (Loops.empty())
      return false;
    PostDominators.recalculate(Fn);
    return sequence(&Fn.getEntryBlock(), nullptr, nullptr, Regions, 0) &&
           Seen.size() == Fn.size() && Edges == ExpectedEdges;
  }
};

} // namespace

void LLVMCWriter::coalesceScalarPhiNames(llvm::Function &Fn) {
  // Text substitutions have their own provenance rules. Until those are
  // represented as operands, only coalesce a projection expressed by SSA.
  if (!ValueTexts.empty() || !OmittedInlined.empty())
    return;
  std::vector<const llvm::PHINode *> Phis;
  llvm::DenseMap<const llvm::Value *, unsigned> IDs;
  llvm::DenseMap<const llvm::BasicBlock *, unsigned> Blocks;
  for (const auto &BB : Fn) {
    Blocks[&BB] = Blocks.size();
    for (const auto &Phi : BB.phis()) {
      if (Analysis.Inlinable.count(&Phi) || Phis.size() == 128)
        return;
      IDs[&Phi] = Phis.size();
      Phis.push_back(&Phi);
    }
  }
  if (Phis.size() < 2)
    return;
  using Bits = llvm::BitVector;
  const unsigned N = Phis.size();
  struct BlockFacts {
    Bits Uses, Defs, Live;
    std::vector<std::pair<unsigned, Bits>> Edges;
    explicit BlockFacts(unsigned N) : Uses(N), Defs(N), Live(N) {}
  };
  std::vector<BlockFacts> Facts(Fn.size(), BlockFacts(N));
  unsigned Work = 0;
  auto Reads = [&](auto &&Self, const llvm::Value *V, Bits &Into,
                   unsigned Depth) -> bool {
    if (++Work > 1048576 || Depth > 64)
      return false;
    if (auto It = IDs.find(V); It != IDs.end()) {
      Into.set(It->second);
    } else if (const auto *Inst = llvm::dyn_cast<llvm::Instruction>(V);
               Inst && Analysis.Inlinable.count(Inst)) {
      for (const auto &Op : Inst->operands())
        if (!Self(Self, Op, Into, Depth + 1))
          return false;
    }
    return true;
  };
  for (const auto &BB : Fn) {
    auto &F = Facts[Blocks.lookup(&BB)];
    for (const auto &Phi : BB.phis())
      F.Defs.set(IDs.lookup(&Phi));
    for (const auto &Inst : BB) {
      if (llvm::isa<llvm::PHINode>(Inst) || !instructionIsPrinted(Inst))
        continue;
      for (const auto &Op : Inst.operands())
        if (!Reads(Reads, Op, F.Uses, 0))
          return;
    }
    for (const auto *To : llvm::successors(&BB)) {
      Bits EdgeUses(N);
      for (const auto &Phi : To->phis())
        if (!Reads(Reads, Phi.getIncomingValueForBlock(&BB), EdgeUses, 0))
          return;
      F.Edges.emplace_back(Blocks.lookup(To), std::move(EdgeUses));
    }
  }
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (auto &F : Facts) {
      if (++Work > 1048576)
        return;
      Bits Live = F.Uses;
      for (const auto &[To, Reads] : F.Edges) {
        Bits Before = Facts[To].Live;
        Before.reset(Facts[To].Defs);
        Before |= Reads;
        Live |= Before;
      }
      if (Live != F.Live) {
        F.Live = std::move(Live);
        Changed = true;
      }
    }
  }
  std::vector<Bits> Interference(N, Bits(N));
  for (const auto &F : Facts) {
    // PHI definitions publish simultaneously, including a value whose old
    // lifetime just ended. Account for actual printed, recursively inlined
    // operands rather than only immediate SSA uses.
    Bits Live = F.Live;
    Live |= F.Defs;
    for (int I : Live.set_bits())
      Interference[I] |= Live;
  }
  std::vector<unsigned> Groups(N);
  for (unsigned I = 0; I != N; ++I)
    Groups[I] = I;
  for (unsigned I = 0; I != N; ++I) {
    for (const auto &Incoming : Phis[I]->incoming_values()) {
      auto It = IDs.find(Incoming);
      if (It == IDs.end() || Phis[I]->getType() != Incoming->getType())
        continue;
      const unsigned A = Groups[I], B = Groups[It->second];
      if (A == B)
        continue;
      bool Conflict = false;
      for (unsigned X = 0; X != N; ++X)
        for (unsigned Y = 0; Y != N; ++Y) {
          if (++Work > 1048576)
            return;
          if (Groups[X] == A && Groups[Y] == B && Interference[X].test(Y))
            Conflict = true;
        }
      if (!Conflict)
        for (auto &Group : Groups)
          if (Group == A || Group == B)
            Group = std::min(A, B);
    }
  }
  // Publish only after completing every analysis and budget check.
  for (unsigned I = 0; I != N; ++I)
    if (Groups[I] != I)
      ValNames[Phis[I]] = getName(Phis[Groups[I]]);
  InlineCache.clear();
}

bool LLVMCWriter::tryWriteScalarRegions(llvm::Function &Fn, int Indent) {
  if (DebugFn)
    return false;
  std::vector<ScalarRegion> Regions;
  if (!ScalarRegionPlan(Fn).build(Regions, Dominators))
    return false;
  // Funnel shifts have total, typed C helpers. A single same-block use and
  // leaf operands need neither a snapshot nor an additional expression-tree
  // materialization boundary. Ordinary calls keep their original ordering.
  for (auto &BB : Fn)
    for (auto &Inst : BB) {
      auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&Inst);
      if (!Call || !Call->hasOneUse() ||
          (Call->getIntrinsicID() != llvm::Intrinsic::fshl &&
           Call->getIntrinsicID() != llvm::Intrinsic::fshr))
        continue;
      auto *Use = llvm::dyn_cast<llvm::Instruction>(*Call->user_begin());
      if (!Use || Use->getParent() != &BB || llvm::isa<llvm::PHINode>(Use))
        continue;
      bool Leaves = true;
      for (const auto &Arg : Call->args())
        Leaves &= llvm::isa<llvm::Argument, llvm::ConstantInt, llvm::PHINode>(
            Arg.get());
      if (Leaves)
        Analysis.Inlinable.insert(Call);
    }
  coalesceScalarPhiNames(Fn);
  // This contract uses the LLVM function's own unsigned scalar C objects.
  // Debug/image and composed text projections need their own type evidence.
  UseScalarExpressionTypes =
      !Dbg && !Img && ValueTexts.empty() && OmittedInlined.empty();
  for (const auto &BB : Fn)
    for (const auto &Phi : BB.phis())
      if (Analysis.Inlinable.count(&Phi))
        UseScalarExpressionTypes = false;

  auto ClearPath = [&]() {
    // Layout facts from one arm/backedge cannot substitute values on another.
    KnownImmediates.clear();
    InlineCache.clear();
    AllocaImmediates.clear();
    AllocaLastValues.clear();
    AfterCxxThrow = false;
  };
  auto WriteBlock = [&](llvm::BasicBlock *BB, int Level) {
    ClearPath();
    for (auto &Inst : *BB)
      if (!Inst.isTerminator() && !llvm::isa<llvm::PHINode>(Inst))
        writeInstruction(Inst, Level);
  };
  auto HasHeaderWork = [&](const ScalarRegion &Region) {
    ClearPath();
    for (const auto &Inst : *Region.Block)
      if (!Inst.isTerminator() && instructionIsPrinted(Inst))
        return true;
    return false;
  };
  // A constant-seeded, uniquely latched counter can put just its assignment
  // into a for clause. Other carried values still publish on the same edge,
  // before the counter changes, so their old-counter reads need no snapshot.
  std::map<const llvm::BasicBlock *, const ScalarRegion *> ForLoops;
  auto PlanCounters = [&](auto &&Self, auto &Sequence) -> void {
    for (auto &Region : Sequence) {
      if (Region.K == ScalarRegion::Kind::Loop && Region.EntryPred &&
          !HasHeaderWork(Region) &&
          llvm::isa<llvm::UncondBrInst>(Region.Latch->getTerminator())) {
        auto *Br = llvm::cast<llvm::CondBrInst>(Region.Block->getTerminator());
        auto *Cmp = llvm::dyn_cast<llvm::ICmpInst>(Br->getCondition());
        if (Cmp)
          for (const auto &Phi : Region.Block->phis()) {
            if (Phi.getNumIncomingValues() != 2 ||
                Analysis.Inlinable.count(&Phi) ||
                (Cmp->getOperand(0) != &Phi && Cmp->getOperand(1) != &Phi) ||
                !llvm::isa<llvm::ConstantInt>(
                    Phi.getIncomingValueForBlock(Region.EntryPred)))
              continue;
            auto *Step = llvm::dyn_cast<llvm::BinaryOperator>(
                Phi.getIncomingValueForBlock(Region.Latch));
            if (!Step || !Step->hasOneUse() ||
                !Analysis.Inlinable.count(Step) ||
                (Step->getOpcode() != llvm::Instruction::Add &&
                 Step->getOpcode() != llvm::Instruction::Sub) ||
                Step->getOperand(0) != &Phi ||
                !llvm::isa<llvm::ConstantInt>(Step->getOperand(1)))
              continue;
            Region.Counter = &Phi;
            ForLoops[Region.Block] = &Region;
            break;
          }
      }
      Self(Self, Region.First);
      Self(Self, Region.Second);
    }
  };
  PlanCounters(PlanCounters, Regions);
  auto Condition = [&](const ScalarRegion &Region) {
    auto *Br = llvm::cast<llvm::CondBrInst>(Region.Block->getTerminator());
    if (Region.Invert) {
      if (auto Text = invertedRelationalText(Br->getCondition()))
        return *Text;
      return std::string("!(") + condStr(Br->getCondition()) + ")";
    }
    return condStr(Br->getCondition());
  };
  std::function<void(const std::vector<ScalarRegion> &, int)> Write;
  Write = [&](const auto &Sequence, int Level) {
    for (const auto &Region : Sequence) {
      switch (Region.K) {
      case ScalarRegion::Kind::Block:
        WriteBlock(Region.Block, Level);
        if (auto *Ret =
                llvm::dyn_cast<llvm::ReturnInst>(Region.Block->getTerminator()))
          writeReturn(*Ret, Level);
        break;
      case ScalarRegion::Kind::Edge:
        ClearPath();
        {
          const llvm::PHINode *Deferred = nullptr;
          if (auto It = ForLoops.find(Region.Target); It != ForLoops.end())
            if (Region.Block == It->second->EntryPred ||
                Region.Block == It->second->Latch)
              Deferred = It->second->Counter;
          writePhiCopies(Region.Block, Region.Target, Level,
                         /*ForceMaterialized=*/true, Deferred);
        }
        break;
      case ScalarRegion::Kind::If:
        emitIndent(Level);
        OS << "if (" << Condition(Region) << ") {\n";
        Write(Region.First, Level + 1);
        emitIndent(Level);
        OS << "} else {\n";
        Write(Region.Second, Level + 1);
        emitIndent(Level);
        OS << "}\n";
        break;
      case ScalarRegion::Kind::Loop: {
        ClearPath();
        const bool HeaderWork = HasHeaderWork(Region);
        emitIndent(Level);
        if (Region.Counter) {
          const auto Name = getName(Region.Counter);
          OS << "for (" << Name << " = "
             << valueStr(
                    Region.Counter->getIncomingValueForBlock(Region.EntryPred))
             << "; " << Condition(Region) << "; " << Name << " = "
             << valueStr(Region.Counter->getIncomingValueForBlock(Region.Latch))
             << ") {\n";
        } else {
          OS << "while (" << (HeaderWork ? "1" : Condition(Region)) << ") {\n";
        }
        if (HeaderWork) {
          WriteBlock(Region.Block, Level + 1);
          emitIndent(Level + 1);
          OS << "if (!(" << Condition(Region) << ")) break;\n";
        }
        Write(Region.First, Level + 1);
        emitIndent(Level);
        OS << "}\n";
        Write(Region.Second, Level);
        break;
      }
      }
    }
  };
  Write(Regions, Indent);
  ClearPath();
  UseScalarExpressionTypes = false;
  return true;
}

} // namespace neverd
