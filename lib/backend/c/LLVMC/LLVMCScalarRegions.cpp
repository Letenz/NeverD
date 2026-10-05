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
  enum class Kind { Block, Edge, If, Loop, Repeat, Break } K;
  llvm::BasicBlock *Block = nullptr;
  llvm::BasicBlock *Target = nullptr;
  bool Invert = false;
  std::vector<ScalarRegion> First;
  std::vector<ScalarRegion> Second;
  llvm::BasicBlock *EntryPred = nullptr;
  llvm::BasicBlock *Latch = nullptr;
  const llvm::PHINode *Counter = nullptr;
  const llvm::Loop *Scope = nullptr;
  bool ScopedCounter = false;
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
                unsigned Depth, bool StartAtStop = false) {
    if (Depth > 64)
      return false;
    bool First = StartAtStop;
    while (BB && (BB != Stop || First)) {
      First = false;
      if (++Work > 65536 || (Scope && !Scope->contains(BB)) ||
          !Seen.insert(BB).second)
        return false;
      const llvm::Loop *Loop = Loops.getLoopFor(BB);
      if (Loop && Loop->getHeader() == BB && Loop != Scope) {
        auto *Branch = llvm::dyn_cast<llvm::CondBrInst>(BB->getTerminator());
        if (Loop->getExitingBlock() != BB) {
          auto *Exit = Loop->getExitBlock();
          if (!Exit || !Loop->getExitingBlock() || !Loop->getLoopLatch() ||
              Loop->getParentLoop() != Scope)
            return false;
          ScalarRegion Region{
              ScalarRegion::Kind::Repeat, BB, Exit, false, {}, {}};
          Region.Scope = Loop;
          Region.EntryPred = Loop->getLoopPredecessor();
          Region.Latch = Loop->getLoopLatch();
          // The body owns the header's instructions and successor edges too.
          // Its first visit starts at Stop; only a later backedge ends it.
          Seen.erase(BB);
          if (!sequence(BB, BB, Loop, Region.First, Depth + 1, true))
            return false;
          Into.push_back(std::move(Region));
          BB = Exit;
          continue;
        }
        // A header exit retains the direct while/for form. Incomplete or
        // multi-exit regions keep the existing goto projection.
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
        Region.Scope = Loop;
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
      // A repeat region keeps the sole internal exit at its original point.
      // The taken exit's parallel copies must run before the lexical break.
      if (Scope && BB == Scope->getExitingBlock()) {
        const bool TrueBody = Scope->contains(Branch->getSuccessor(0));
        auto *Body = Branch->getSuccessor(TrueBody ? 0 : 1);
        auto *Exit = Branch->getSuccessor(TrueBody ? 1 : 0);
        if (!Scope->contains(Body) || Scope->contains(Exit) ||
            Exit != Scope->getExitBlock())
          return false;
        ScalarRegion Region{
            ScalarRegion::Kind::Break, BB, Exit, TrueBody, {}, {}};
        if (!edge(Region.First, BB, Exit) || !edge(Region.Second, BB, Body))
          return false;
        Into.push_back(std::move(Region));
        BB = Body;
        continue;
      }
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
  // Region scopes borrow LoopInfo objects through the entire rendering pass.
  ScalarRegionPlan Plan(Fn);
  if (!Plan.build(Regions, Dominators))
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
  const llvm::PHINode *EntryDeclaration = nullptr;
  if (UseScalarExpressionTypes) {
    // A short local must not hide an external or recursive callee. Reserve
    // the same C identifier used by the call writer before assigning roles.
    UsedNames.insert(functionIdentifier(Fn));
    for (const auto &BB : Fn)
      for (const auto &Inst : BB)
        if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst))
          UsedNames.insert(functionIdentifier(*Call->getCalledFunction()));
    // These are generated C roles, not recovered debug names. Rename the
    // complete coalesced group so copies continue to refer to one object.
    std::map<std::string, std::vector<const llvm::Value *>> NameGroups;
    for (const auto &[Value, Name] : ValNames)
      NameGroups[Name].push_back(Value);
    auto Rename = [&](const llvm::Value *Value, const char *Hint) {
      const auto OldName = getName(Value);
      auto Found = NameGroups.find(OldName);
      if (Found == NameGroups.end() ||
          !llvm::all_of(Found->second, [&](const llvm::Value *Other) {
            return llvm::isa<llvm::PHINode>(Other) &&
                   Other->getType() == Value->getType();
          }))
        return std::string();
      const auto Name = freshVar(Hint);
      auto Group = std::move(Found->second);
      NameGroups.erase(Found);
      for (const auto *Other : Group)
        ValNames[Other] = Name;
      NameGroups[Name] = std::move(Group);
      return Name;
    };
    const llvm::PHINode *Result = nullptr;
    bool OneResult = true;
    for (const auto &BB : Fn)
      if (const auto *Ret =
              llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
        const auto *Phi =
            llvm::dyn_cast_or_null<llvm::PHINode>(Ret->getReturnValue());
        if (!Phi || (Result && Result != Phi))
          OneResult = false;
        Result = Phi;
      }
    if (OneResult && Result) {
      const auto Name = Rename(Result, "result");
      // A direct entry edge dominates all following source statements. An
      // immutable leaf seed can declare the returned carrier at that edge,
      // provided the whole coalesced group starts after entry and exactly one
      // PHI on the edge owns this name. Other declarations retain their scope.
      const auto *Entry = &Fn.getEntryBlock();
      const auto *Branch =
          llvm::dyn_cast<llvm::UncondBrInst>(Entry->getTerminator());
      if (!Name.empty() && Branch) {
        auto *Next = Branch->getSuccessor(0);
        bool Confined = true;
        for (const auto *Value : NameGroups.at(Name)) {
          const auto *Phi = llvm::cast<llvm::PHINode>(Value);
          Confined &= Phi->getParent() != Entry &&
                      Dominators.dominates(Next, Phi->getParent());
        }
        unsigned Owners = 0;
        for (const auto &Phi : Next->phis())
          if (getName(&Phi) == Name) {
            ++Owners;
            const auto *Seed = Phi.getIncomingValueForBlock(Entry);
            if (Confined && llvm::isa<llvm::Argument, llvm::ConstantInt>(Seed))
              EntryDeclaration = &Phi;
          }
        if (Owners != 1)
          EntryDeclaration = nullptr;
      }
    }

    // A direct LLVM use can inline through another value and be printed
    // outside the loop. Follow those uses through every inlined expression;
    // a materialized result is the boundary that retains its own lifetime.
    unsigned ScopeWork = 0;
    auto Confined = [&](const ScalarRegion &Region) {
      auto Group = NameGroups.find(getName(Region.Counter));
      if (Group == NameGroups.end())
        return false;
      llvm::SmallPtrSet<const llvm::Value *, 32> Seen;
      llvm::SmallVector<const llvm::Value *, 32> Pending(Group->second.begin(),
                                                         Group->second.end());
      while (!Pending.empty()) {
        const auto *Value = Pending.pop_back_val();
        if (++ScopeWork > 65536)
          return false;
        if (!Seen.insert(Value).second)
          continue;
        const auto *Inst = llvm::dyn_cast<llvm::Instruction>(Value);
        if (!Inst || !Region.Scope->contains(Inst))
          return false;
        for (const auto *User : Value->users()) {
          if (++ScopeWork > 65536)
            return false;
          const auto *Use = llvm::dyn_cast<llvm::Instruction>(User);
          if (!Use || !Region.Scope->contains(Use))
            return false;
          if (Analysis.Inlinable.count(Use) &&
              !MaterializedExpressions.count(Use))
            Pending.push_back(Use);
        }
      }
      return true;
    };
    auto ScopeCounters = [&](auto &&Self, auto &Sequence) -> void {
      for (auto &Region : Sequence) {
        if (Region.Counter && Confined(Region)) {
          const auto Name = Rename(Region.Counter, "i");
          if (!Name.empty()) {
            ScopedScalarNames.insert(Name);
            Region.ScopedCounter = true;
          }
        }
        Self(Self, Region.First);
        Self(Self, Region.Second);
      }
    };
    ScopeCounters(ScopeCounters, Regions);
  }
  auto Condition = [&](const ScalarRegion &Region) {
    auto *Br = llvm::cast<llvm::CondBrInst>(Region.Block->getTerminator());
    if (auto Text = scalarConditionText(Br->getCondition(), Region.Invert))
      return *Text;
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
                         /*ForceMaterialized=*/true, Deferred,
                         Region.Block == &Fn.getEntryBlock() ? EntryDeclaration
                                                             : nullptr);
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
      case ScalarRegion::Kind::Repeat:
        emitIndent(Level);
        OS << "while (1) {\n";
        Write(Region.First, Level + 1);
        emitIndent(Level);
        OS << "}\n";
        break;
      case ScalarRegion::Kind::Break:
        emitIndent(Level);
        OS << "if (" << Condition(Region) << ") {\n";
        Write(Region.First, Level + 1);
        emitIndent(Level + 1);
        OS << "break;\n";
        emitIndent(Level);
        OS << "}\n";
        Write(Region.Second, Level);
        break;
      case ScalarRegion::Kind::Loop: {
        ClearPath();
        const bool HeaderWork = HasHeaderWork(Region);
        emitIndent(Level);
        if (Region.Counter) {
          const auto Name = getName(Region.Counter);
          OS << "for (";
          if (Region.ScopedCounter)
            OS << typeToCLLVM(Region.Counter->getType()) << " ";
          OS << Name << " = "
             << valueStr(
                    Region.Counter->getIncomingValueForBlock(Region.EntryPred))
             << "; " << Condition(Region) << "; ";
          const auto *Step =
              Region.Counter->getIncomingValueForBlock(Region.Latch);
          if (auto Update = scalarUpdateText(Region.Counter, Step))
            OS << *Update;
          else
            OS << Name << " = " << valueStr(Step);
          OS << ") {\n";
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
