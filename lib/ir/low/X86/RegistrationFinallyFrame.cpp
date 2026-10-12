//===- RegistrationFinallyFrame.cpp - Returning finally frame effects -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

#include <deque>

namespace neverd::registration_state {

bool RegistrationStateSolver::runLocalFinally(va_t Target, FrameState &Parent,
                                              bool OrdinaryInvocation) {
  const auto Entry = Entries.find(Target);
  const auto ParentSP = Parent.load(*Chain.RegistrationOffset - 8, 4).Offset;
  if (Entry == Entries.end() || !ParentSP ||
      *ParentSP >= *Chain.RegistrationOffset - 8 ||
      !charge(Parent.cellCount() + Parent.OtherRegisterBytes.size() + 10))
    return false;
  FrameState Initial;
  if (OrdinaryInvocation)
    Initial = Parent;
  else {
    Initial.Cells = Parent.Cells;
    for (auto &Register : Initial.Registers)
      Register.MayBeFrame = true;
    Initial.OtherRegistersMayBeFrame = true;
    Initial.Registers[x86reg::RBP / x86reg::GeneralRegStride] =
        FrameValue::frame(0);
  }
  Initial.enterCallback(Target);
  std::map<size_t, FrameState> Incoming{{Entry->second, Initial}};
  std::deque<size_t> Pending{Entry->second};
  std::set<size_t> Queued{Entry->second};
  std::optional<FrameState> Returned;
  while (!Pending.empty()) {
    const size_t I = Pending.front();
    Pending.pop_front();
    Queued.erase(I);
    FrameState State = Incoming.at(I);
    FrameTransfer Transfer(State, *Chain.RegistrationOffset);
    const auto &Block = Function.Blocks[I];
    if (OrdinaryInvocation) {
      if (!charge(Block.Ops.size() + Block.InstructionBoundaries.size() +
                  Block.Succs.size() + 1))
        return false;
      if (auto Error = validateLowInstructionBoundaries(
              Block, LowInstructionBoundaryRequirement::Required)) {
        llvm::consumeError(std::move(Error));
        return false;
      }
      for (const auto &Insn : Block.InstructionBoundaries)
        if (Insn.Mode != InstructionMode::Default ||
            Insn.TargetMode != LowInstructionTargetMode::Preserve ||
            (&Insn != &Block.InstructionBoundaries.back() &&
             Insn.Control != LowInstructionControl::None))
          return false;
      std::set<va_t> Actual, Expected;
      for (int Next : Block.Succs) {
        const auto Successor = Index.find(Next);
        if (Successor == Index.end() ||
            !Actual.insert(Function.Blocks[Successor->second].StartAddr).second)
          return false;
      }
      const auto Control = Block.InstructionBoundaries.back().Control;
      if (Control == LowInstructionControl::Branch && !Block.Ops.empty()) {
        const auto &Branch = Block.Ops.back();
        const bool Conditional = Branch.Opcode == NdOp::COND_BR;
        if ((!Conditional && Branch.Opcode != NdOp::BRANCH) ||
            Branch.NumInputs != (Conditional ? 2 : 1) ||
            !Branch.Inputs[0].isConst() || Branch.Inputs[0].Size != 4)
          return false;
        Expected.insert(Branch.Inputs[0].Offset);
        if (Conditional)
          Expected.insert(Block.EndAddr);
      } else if (Control == LowInstructionControl::None)
        Expected.insert(Block.EndAddr);
      else if (Control != LowInstructionControl::Return)
        return false;
      if (Actual != Expected)
        return false;
    }
    bool HasReturn = false;
    for (const auto &Op : Block.Ops) {
      if (!charge(1 + State.cellCount() + Op.Output.Size) ||
          Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::INDIR_BR ||
          Op.MemoryOrdering != NdMemoryOrdering::None)
        return false;
      const auto Source = Boundaries.find(Op.Addr);
      const size_t OpIndex = &Op - Block.Ops.data();
      if (Op.Seq < 0 || Source == Boundaries.end() ||
          Source->second.first != Block.Id ||
          Source->second.second.FirstOp > OpIndex ||
          Source->second.second.OpCount <=
              OpIndex - Source->second.second.FirstOp ||
          Source->second.second.FirstOp > Block.Ops.size() ||
          Source->second.second.OpCount >
              Block.Ops.size() - Source->second.second.FirstOp)
        return false;
      Transfer.beginInstruction(Op.Addr);
      if (Op.Opcode == NdOp::RETURN) {
        if (!callbackReturnInstruction(I, Op) ||
            State.CallbackEntry != Target || !State.callbackStackIsRestored(0))
          return false;
        if (!Returned)
          Returned = State;
        else if (!charge(Returned->cellCount() + State.cellCount() + 10))
          return false;
        else
          Returned->merge(State);
        HasReturn = true;
        break;
      }
      const auto Memory = lowMemoryOperands(Op);
      if (Memory.Address) {
        if (!Memory.Complete ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            (Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE))
          return false;
        const auto Address = Transfer.read(*Memory.Address);
        if (Address.CallbackAddress) {
          if (!charge(Memory.AccessSize) ||
              !State.callbackMemoryIsPrivate(*Address.CallbackAddress,
                                             Memory.AccessSize,
                                             Op.Opcode == NdOp::LOAD))
            return false;
          if (Op.Opcode == NdOp::STORE)
            State.storeCallback(Address.CallbackAddress->Offset,
                                Memory.AccessSize,
                                Transfer.read(*Memory.StoredValue));
        } else if (!Address.Offset || *Address.Offset < *ParentSP ||
                   int64_t(*Address.Offset) + Memory.AccessSize > 0) {
          return false;
        } else if (Op.Opcode == NdOp::STORE) {
          // The runtime owns SavedESP, exception pointers and the complete
          // registration record. A finally that can change them needs a
          // separate nonlocal-transition proof, not an ordinary return fact.
          if (!frameObjectIsDisjointFromAdministration(
                  *Address.Offset,
                  int64_t(*Address.Offset) + Memory.AccessSize))
            return false;
          const auto Value = Transfer.read(*Memory.StoredValue);
          if (Value.CallbackAddress || (Value.MayBeFrame && !Value.Offset))
            return false;
          State.store(*Address.Offset, Memory.AccessSize, Value);
        }
      }
      Transfer.write(Op, Transfer.evaluate(Op, true));
      if (Op.Output.isReg() && Op.Output.Offset < x86reg::RSP + 4 &&
          x86reg::RSP < Op.Output.Offset + Op.Output.Size) {
        const auto SP = State.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                            .CallbackAddress;
        if (!SP || SP->Entry != Target || SP->Offset > 0 ||
            !charge(State.CallbackCells.size() +
                    State.InitializedCallbackBytes.size()))
          return false;
        State.trimCallbackCells();
      }
    }
    if (HasReturn) {
      if (!Block.Succs.empty())
        return false;
      continue;
    }
    if (Block.Succs.empty())
      return false;
    for (int Next : Block.Succs) {
      const auto Successor = Index.find(Next);
      if (Successor == Index.end() || !charge(State.cellCount() + 10))
        return false;
      auto [It, New] = Incoming.emplace(Successor->second, State);
      if ((New || It->second.merge(State)) &&
          Queued.insert(Successor->second).second)
        Pending.push_back(Successor->second);
    }
  }
  if (!Returned)
    return false;
  Parent.Cells = std::move(Returned->Cells);
  if (OrdinaryInvocation) {
    // A source finally is not an ABI-preserving ordinary function. Retain
    // its actual register effects; the caller separately restores the SP
    // after the checked near RET has consumed its own untouched return PC.
    Parent.Registers = Returned->Registers;
    Parent.OtherRegisterBytes = std::move(Returned->OtherRegisterBytes);
    Parent.OtherRegistersMayBeFrame = Returned->OtherRegistersMayBeFrame;
  }
  return true;
}

} // namespace neverd::registration_state
