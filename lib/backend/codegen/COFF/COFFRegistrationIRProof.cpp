//===- COFFRegistrationIRProof.cpp - Shared source execution proof -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "COFFRegistrationIRProof.h"

#include "neverd/Limits.h"

#include "llvm/IR/CFG.h"
namespace neverd::coff_registration {
bool matchesSourceOperation(const llvm::Instruction &I, va_t Owner,
                            va_t Address, uint8_t Kind) {
  const auto *MD =
      I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
  return MD && MD->getNumOperands() == 5 &&
         metadataInteger(*MD, 0, 64) == Owner &&
         metadataInteger(*MD, 1, 64) == Address &&
         metadataInteger(*MD, 2, 32).has_value() &&
         metadataInteger(*MD, 3, 32).has_value() &&
         metadataInteger(*MD, 4, 8) == Kind;
}
llvm::Expected<std::map<int, SourceSegment>> validateSourceSegments(
    const llvm::Function &Parent, const MedFunc &Source,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks,
    llvm::ArrayRef<const llvm::Function *> Functions, bool ReachedCxxSource) {
  using Key = std::pair<va_t, int>;
  struct Event {
    Key Identity;
    uint8_t Kind;
  };
  std::map<int, const MedBlock *> Blocks;
  std::map<int, std::vector<Event>> Expected;
  std::map<int, const llvm::Function *> Owners;
  size_t Work = 0;
  auto Charge = [&](size_t Amount = 1) {
    if (Amount > limits::kMaxRegistrationEHStateWork - Work)
      return false;
    Work += Amount;
    return true;
  };
  std::map<std::pair<va_t, va_t>, int> ReachedBlocks;
  if (ReachedCxxSource) {
    if (!Source.RegistrationStates)
      return rejectIR("C++ execution source has no checked states");
    for (const auto &State : Source.RegistrationStates->Blocks)
      if (State.Reached &&
          !ReachedBlocks
               .emplace(std::make_pair(State.Range.Begin, State.Range.End),
                        State.BlockId)
               .second)
        return rejectIR("C++ reached execution range is duplicated");
  }
  for (const auto &Block : Source.Blocks) {
    int Id = Block.Id;
    if (ReachedCxxSource) {
      const auto State = ReachedBlocks.find({Block.StartAddr, Block.EndAddr});
      if (State == ReachedBlocks.end())
        continue;
      Id = State->second;
    }
    if (!Blocks.emplace(Id, &Block).second)
      return rejectIR("source execution block identity is duplicated");
    Owners[Id] = &Parent;
    for (const auto &Op : Block.Ops) {
      if (!Charge())
        return rejectIR("source execution replay exceeds its work budget");
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
        Expected[Id].push_back(
            {{Op.Addr, Op.OriginSeq}, uint8_t(Op.Opcode == NdOp::STORE)});
      else if (Op.Opcode == NdOp::CALL)
        Expected[Id].push_back(
            {{Op.Addr, Op.OriginSeq},
             uint8_t(Source.RegistrationStates &&
                             Source.RegistrationStates->cookieCheck(
                                 Op.Addr, Op.OriginSeq)
                         ? 3
                         : 2)});
      if (ReachedCxxSource && Source.RegistrationStates) {
        const auto *Effect =
            Source.RegistrationStates->callFrameEffect(Op.Addr, Op.OriginSeq);
        if (Op.Opcode == NdOp::CALL && Effect && Effect->DoesNotReturn)
          break;
      }
    }
  }
  std::set<int> CallbackBlocks;
  std::map<const llvm::Function *, int> CallbackEntries;
  for (const auto &[Identity, Function] : Callbacks) {
    const MedBlock *Entry = nullptr;
    for (const auto &[Id, Block] : Blocks)
      if (Block->StartAddr == Identity.first)
        Entry = Block;
    if (!Entry)
      return rejectIR("callback has no checked source execution entry");
    CallbackEntries.emplace(Function, Entry->Id);
    std::vector<int> Pending{Entry->Id};
    std::set<int> Seen;
    while (!Pending.empty()) {
      const int Id = Pending.back();
      Pending.pop_back();
      if (!Seen.insert(Id).second)
        continue;
      if (!Blocks.count(Id) || !CallbackBlocks.insert(Id).second)
        return rejectIR("callback source execution ownership overlaps");
      if (!Charge(1 + Blocks.at(Id)->Succs.size()))
        return rejectIR(
            "callback source execution replay exceeds its work budget");
      Owners[Id] = Function;
      Pending.insert(Pending.end(), Blocks.at(Id)->Succs.begin(),
                     Blocks.at(Id)->Succs.end());
    }
  }
  std::map<int, SourceSegment> Segments;
  std::set<const llvm::Instruction *> Events;
  for (const auto *Function : Functions)
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        if (!Charge())
          return rejectIR("source execution closure exceeds its work budget");
        if (I.getMetadata(windows_eh_md::RegistrationOperationAttachment))
          Events.insert(&I);
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationBlockAttachment);
        if (!MD)
          continue;
        const auto *Anchor = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
        auto Id = metadataInteger(*MD, 3, 32);
        auto Enter = metadataInteger(*MD, 4, 1);
        const auto *Terminal = llvm::dyn_cast<llvm::CallBase>(&I);
        const bool TerminalBoundary = ReachedCxxSource && Enter && !*Enter &&
                                      Terminal && Terminal->doesNotReturn() &&
                                      !Terminal->isInlineAsm();
        const bool PlainBoundary =
            Anchor && Anchor->getIntrinsicID() == llvm::Intrinsic::sideeffect &&
            !Anchor->arg_size() && !Anchor->getNumOperandBundles();
        if (MD->getNumOperands() != 5 ||
            (!PlainBoundary && !TerminalBoundary) ||
            metadataInteger(*MD, 0, 64) != Source.Entry || !Id ||
            *Id > INT_MAX || !Blocks.count(int(*Id)) || !Enter ||
            Owners.at(int(*Id)) != Function ||
            metadataInteger(*MD, 1, 64) != Blocks.at(int(*Id))->StartAddr ||
            metadataInteger(*MD, 2, 64) != Blocks.at(int(*Id))->EndAddr) {
          std::string Detail;
          llvm::raw_string_ostream Stream(Detail);
          Stream << "source execution segment has a malformed identity: ";
          I.print(Stream);
          Stream << "; actual owner/range/block "
                 << metadataInteger(*MD, 0, 64).value_or(0) << "/["
                 << metadataInteger(*MD, 1, 64).value_or(0) << ", "
                 << metadataInteger(*MD, 2, 64).value_or(0) << ")/"
                 << Id.value_or(UINT32_MAX);
          if (Id && *Id <= INT_MAX && Blocks.count(int(*Id)))
            Stream << "; expected [" << Blocks.at(int(*Id))->StartAddr << ", "
                   << Blocks.at(int(*Id))->EndAddr << ")";
          return rejectIR(Detail);
        }
        auto &Segment = Segments[int(*Id)];
        auto &Position = *Enter ? Segment.Enter : Segment.Exit;
        if (Position)
          return rejectIR("source execution segment boundary is duplicated");
        Position = &I;
      }
  if (Segments.size() != Blocks.size())
    return rejectIR("source execution segment set is incomplete");
  std::set<const llvm::Instruction *> SeenEvents;
  for (const auto &[Id, Segment] : Segments) {
    if (!Segment.Enter || !Segment.Exit)
      return rejectIR(
          "source execution segment has no exact boundaries for block " +
          llvm::Twine(Id));
    const llvm::Instruction *I = next(*Segment.Enter);
    std::set<const llvm::BasicBlock *> SeenParts;
    size_t EventIndex = 0;
    const bool TerminalExit = ReachedCxxSource &&
                              llvm::isa<llvm::CallBase>(Segment.Exit) &&
                              !llvm::isa<llvm::IntrinsicInst>(Segment.Exit);
    while (I != Segment.Exit ||
           (TerminalExit && EventIndex < Expected[Id].size())) {
      if (!Charge())
        return rejectIR("source execution segment exceeds its work budget");
      if (!I || I->getFunction() != Owners.at(Id) ||
          (I != Segment.Exit &&
           I->getMetadata(windows_eh_md::RegistrationBlockAttachment)))
        return rejectIR(
            "source execution segment crosses another source block");
      if (const auto *MD =
              I->getMetadata(windows_eh_md::RegistrationOperationAttachment)) {
        auto Address = metadataInteger(*MD, 1, 64);
        auto Seq = metadataInteger(*MD, 2, 32);
        auto Kind = metadataInteger(*MD, 4, 8);
        const auto &SourceEvents = Expected[Id];
        const auto *Load = llvm::dyn_cast<llvm::LoadInst>(I);
        const auto *Store = llvm::dyn_cast<llvm::StoreInst>(I);
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(I);
        const auto *CookieCheck = llvm::dyn_cast<llvm::IntrinsicInst>(I);
        if (MD->getNumOperands() != 5 ||
            metadataInteger(*MD, 0, 64) != Source.Entry ||
            metadataInteger(*MD, 3, 32) != uint32_t(Id) || !Address || !Seq ||
            *Seq > INT_MAX || !Kind || EventIndex >= SourceEvents.size() ||
            SourceEvents[EventIndex].Identity != Key{*Address, int(*Seq)} ||
            SourceEvents[EventIndex].Kind != *Kind ||
            (*Kind == 0 && (!Load || Load->isAtomic() || !Load->isVolatile() ||
                            Load->getAlign() != llvm::Align(1))) ||
            (*Kind == 1 &&
             (!Store || Store->isAtomic() || !Store->isVolatile() ||
              Store->getAlign() != llvm::Align(1))) ||
            (*Kind == 2 && (!Call || Call->isInlineAsm())) ||
            (*Kind == 3 &&
             (!CookieCheck ||
              CookieCheck->getIntrinsicID() != llvm::Intrinsic::sideeffect ||
              CookieCheck->arg_size() ||
              CookieCheck->getNumOperandBundles())) ||
            !SeenEvents.insert(I).second)
          return rejectIR("source memory or call changed its execution segment "
                          "or occurrence order");
        ++EventIndex;
      }
      if (I == Segment.Exit)
        break;
      if (I->isTerminator()) {
        const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(I);
        if (!Invoke || Owners.at(Id) != &Parent ||
            !SeenParts.insert(I->getParent()).second ||
            Invoke->getNormalDest()->getSinglePredecessor() != I->getParent())
          return rejectIR(
              "source execution segment bypasses its original exit");
        I = &*Invoke->getNormalDest()->getFirstInsertionPt();
      } else
        I = next(*I);
    }
    if (EventIndex != Expected[Id].size())
      return rejectIR(
          "source execution segment has an incomplete operation set");
    if (Owners.at(Id) != &Parent) {
      if (Segment.Enter->getParent() != Segment.Exit->getParent())
        return rejectIR("callback source execution segment was split");
      std::set<const llvm::BasicBlock *> ExpectedSuccessors, ActualSuccessors;
      for (int Successor : Blocks.at(Id)->Succs) {
        auto It = Segments.find(Successor);
        if (It == Segments.end() || !It->second.Enter ||
            Owners.at(Successor) != Owners.at(Id))
          return rejectIR("callback edge leaves its source execution closure");
        ExpectedSuccessors.insert(It->second.Enter->getParent());
      }
      const auto *Term = Segment.Exit->getParent()->getTerminator();
      for (unsigned Index = 0; Index < Term->getNumSuccessors(); ++Index)
        ActualSuccessors.insert(Term->getSuccessor(Index));
      if (ExpectedSuccessors != ActualSuccessors)
        return rejectIR(
            "callback normal flow changed its source execution graph");
    }
  }
  if (Events != SeenEvents)
    return rejectIR(
        "source operation is outside its authenticated execution segment");
  for (const auto &[Function, Entry] : CallbackEntries) {
    const auto *Branch = llvm::dyn_cast<llvm::UncondBrInst>(
        Function->getEntryBlock().getTerminator());
    if (!Branch ||
        Branch->getSuccessor(0) != Segments.at(Entry).Enter->getParent())
      return rejectIR(
          "callback runtime entry bypasses its source execution entry");
  }
  return Segments;
}
} // namespace neverd::coff_registration
