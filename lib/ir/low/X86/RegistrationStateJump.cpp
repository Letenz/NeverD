//===- RegistrationStateJump.cpp - Invocation-bound EH3 jump transfers ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd::registration_state {

void RegistrationStateSolver::invalidateJumpRuntimeBindings(
    Domain &State, const FrameValue &Address, uint16_t Width) {
  if (!JumpRuntime || Address.Offset || Address.EntryOffset ||
      Address.CallbackAddress)
    return;
  if (!charge(JumpRuntime->ImportSlots.size() + 1))
    return;
  if (!Address.Constant ||
      llvm::any_of(JumpRuntime->ImportSlots, [&](va_t Slot) {
        return uint64_t(*Address.Constant) < Slot + 4 &&
               Slot < uint64_t(*Address.Constant) + Width;
      })) {
    State.JumpBindingsIntact = false;
    State.JumpCaptures.clear();
  }
}

void RegistrationStateSolver::invalidateJumpCaptures(Domain &State,
                                                     const FrameValue &Address,
                                                     uint16_t Width) {
  if (!charge(State.JumpCaptures.size() + 1))
    return;
  if (!Address.Offset) {
    if (Address.MayBeFrame && !Address.CallbackAddress &&
        !Address.EntryOffset) {
      if (State.Callback)
        for (const auto &[Offset, Capture] : State.JumpCaptures)
          ModifiedCallbackCaptures.insert(Capture.Call);
      State.JumpCaptures.clear();
    }
    return;
  }
  std::erase_if(State.JumpCaptures, [&](const auto &Item) {
    const int64_t Begin = Item.first;
    const bool Overlaps =
        int64_t(*Address.Offset) < Begin + EH3JumpCapture::BufferBytes &&
        Begin < int64_t(*Address.Offset) + Width;
    if (Overlaps && State.Callback)
      ModifiedCallbackCaptures.insert(Item.second.Call);
    return Overlaps;
  });
}

bool RegistrationStateSolver::transferJumpCapture(
    size_t I, Domain &After, const LowOp &Op, const BlockFacts &Facts,
    std::optional<FrameValue> &CallSP) {
  if (!JumpRuntime || !JumpRuntime->Capture || KnownCxx || EH4 ||
      After.Unknown || Facts.Invalid || !After.Parent || After.Callback ||
      !After.JumpBindingsIntact || !After.Installed || After.Uninstalled ||
      !Function.hasCompleteLiftCoverage())
    return false;
  const auto Capture = JumpRuntime->Capture(Op, After.Frame, true, WorkUsed);
  if (!Capture)
    return false;
  const auto Entry = Entries.find(Capture->ReturnPC);
  const auto &Block = Function.Blocks[I];
  if (Entry == Entries.end() || Block.EndAddr != Capture->ReturnPC ||
      Block.Succs.size() != 1 ||
      Block.Succs.front() != Function.Blocks[Entry->second].Id ||
      !charge(After.Frame.cellCount() + After.JumpCaptures.size() * 14 + 40))
    return false;
  invalidateJumpCaptures(After, FrameValue::frame(Capture->BufferOffset),
                         EH3JumpCapture::BufferBytes);
  storeEH3JumpCapture(After.Frame, *Capture);
  After.JumpCaptures.emplace(Capture->BufferOffset, *Capture);
  Result.HasEH3JumpCaptures = true;
  CallSP = FrameValue::frame(Capture->StackOffset);
  return true;
}

bool RegistrationStateSolver::transferJumpResume(size_t I, Domain &After,
                                                 const LowOp &Op,
                                                 BlockFacts &Facts) {
  if (!JumpRuntime || !JumpRuntime->Resume ||
      !JumpRuntime->Resume(Op, WorkUsed))
    return false;
  const auto Refuse = [&] {
    Facts.Invalid = After.Unknown = true;
    After.Levels.clear();
    Result.SEHJumpResumptionsComplete = false;
    return true;
  };
  if (KnownCxx || EH4 || After.Unknown || Facts.Invalid ||
      !After.JumpBindingsIntact || Function.Entry != EH.CodeRange.Begin ||
      !EH.CodeRange.contains(Op.Addr) || Op.Addr > UINT32_MAX - 5 ||
      Op.Addr + 5 > EH.CodeRange.End || !After.Installed || After.Uninstalled ||
      After.Levels.empty() || After.Activation.SearchFilter ||
      Chain.RealignedFrame || Chain.RegistrationOffset != -16 ||
      Chain.TryLevelOffset != -4 || !Function.hasCompleteLiftCoverage() ||
      After.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset != 0)
    return Refuse();
  const auto SP = After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  FrameValue Buffer, Value;
  if (SP.Offset && *SP.Offset <= INT32_MAX - 8) {
    Buffer = After.Frame.load(*SP.Offset, 4);
    Value = After.Frame.load(*SP.Offset + 4, 4);
  } else if (SP.CallbackAddress && After.Callback &&
             After.Activation.Entry == After.Frame.CallbackEntry &&
             SP.CallbackAddress->Entry == After.Frame.CallbackEntry &&
             SP.CallbackAddress->Offset <= -8 &&
             After.Frame.callbackMemoryIsPrivate(*SP.CallbackAddress, 8,
                                                 true)) {
    // Offset zero is the runtime's implicit return slot. The callback memory
    // owner rejects all source reads/writes there; it is never an initialized
    // private spill in CallbackCells.
    Buffer = After.Frame.loadCallback(SP.CallbackAddress->Offset, 4);
    Value = After.Frame.loadCallback(SP.CallbackAddress->Offset + 4, 4);
  } else
    return Refuse();
  if (Buffer.JumpCapture)
    return transferBorrowedJump(I, After, Op, Facts, Buffer, Value);
  const auto Capture = Buffer.Offset ? After.JumpCaptures.find(*Buffer.Offset)
                                     : After.JumpCaptures.end();
  const auto ParentSP = After.Frame.load(-24, 4).Offset;
  if (Capture == After.JumpCaptures.end() || Buffer.ReturnPC ||
      Buffer.CallbackAddress || Buffer.EntryOffset || Buffer.SEHException ||
      Value.MayBeFrame || !ParentSP || *ParentSP > Capture->first ||
      !hasEH3JumpCapture(After.Frame, Capture->second) ||
      !Entries.count(Capture->second.ReturnPC) ||
      (After.Callback && (!SEHControls || !validateSEHInvocationBlock(I))) ||
      !charge(After.Frame.cellCount() + After.JumpCaptures.size() * 14 + 10))
    return Refuse();
  const auto Saved = Capture->second;
  return resumeJumpCapture(After, Facts, Op, Saved, Value);
}

bool RegistrationStateSolver::resumeJumpCapture(Domain &After,
                                                BlockFacts &Facts,
                                                const LowOp &Op,
                                                const EH3JumpCapture &Saved,
                                                const FrameValue &Value,
                                                va_t CalleeVA, va_t ResumeVA) {
  const auto Refuse = [&] {
    Facts.Invalid = After.Unknown = true;
    After.Levels.clear();
    Result.SEHJumpResumptionsComplete = false;
    return true;
  };
  std::optional<FrameState> Joined;
  std::set<int32_t> Stopped;
  for (int32_t Level : After.Levels) {
    FrameState Frame = After.Frame;
    size_t Steps = 0;
    int32_t Walk = Level;
    while (Walk != -1 &&
           (Saved.Level == -1 || uint32_t(Walk) > uint32_t(Saved.Level))) {
      if (!charge(Frame.cellCount() + 1) || Walk < 0 ||
          size_t(Walk) >= Chain.Scopes.size() || ++Steps > Chain.Scopes.size())
        return Refuse();
      const auto &Scope = Chain.Scopes[Walk];
      Walk = Scope.EnclosingLevel;
      Frame.store(-4, 4, FrameValue::constant(uint32_t(Walk)));
      if (Scope.IsFinally) {
        std::vector<RegistrationObjectExtent> Writes;
        if (!runLocalFinally(Scope.HandlerVA, Frame, false, &Writes, &Saved,
                             &Value))
          return Refuse();
        for (const auto &Write : Writes)
          if (Write.Begin <
                  int64_t(Saved.BufferOffset) + EH3JumpCapture::BufferBytes &&
              Saved.BufferOffset < Write.End)
            return Refuse();
      }
    }
    Stopped.insert(Walk);
    if (!Joined)
      Joined = std::move(Frame);
    else if (!charge(Joined->cellCount() + Frame.cellCount() + 10))
      return Refuse();
    else
      Joined->merge(Frame);
  }
  Domain Resume = After;
  Resume.Frame = std::move(*Joined);
  Resume.Frame.leaveCallback();
  Resume.Levels = std::move(Stopped);
  Resume.Activation = {};
  Resume.Parent = true;
  Resume.Callback = Resume.OtherCallback = false;
  Resume.CanDispatch = true;
  Resume.OrdinaryCallStack.reset();
  Resume.StackRestore.reset();
  Resume.SEHCallers.clear();
  constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                    x86reg::RSI};
  for (unsigned R = 0; R != std::size(Registers); ++R)
    Resume.Frame.Registers[Registers[R] / x86reg::GeneralRegStride] =
        Saved.Nonvolatile[R];
  Resume.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::frame(Saved.StackOffset);
  Resume.Frame.Registers[x86reg::RAX / x86reg::GeneralRegStride] =
      Value.Constant == 0 ? FrameValue::constant(1) : Value;
  for (uint64_t R : {x86reg::RCX, x86reg::RDX})
    Resume.Frame.Registers[R / x86reg::GeneralRegStride] = {
        {}, {}, false, true, true};
  Resume.Frame.OtherRegisterBytes.clear();
  Resume.Frame.OtherRegistersMayBeFrame = true;
  RegistrationSEHJumpResume Receipt{Op.Addr,
                                    Op.Seq,
                                    Saved.Call,
                                    Saved.ReturnPC,
                                    After.Activation.Entry.value_or(0),
                                    After.Activation.OrdinaryCall,
                                    Saved.BufferOffset,
                                    Saved.StackOffset,
                                    CalleeVA,
                                    ResumeVA};
  const SEHExitKey Identity{Receipt.EntryVA, Receipt.OrdinaryCall, Op.Addr,
                            Op.Seq};
  const auto [It, New] = JumpResumptions.emplace(Identity, Receipt);
  if (!New && It->second != Receipt)
    return Refuse();
  merge(Entries.at(Saved.ReturnPC), Resume);
  Facts.InvocationAtExit = true;
  return true;
}

} // namespace neverd::registration_state
