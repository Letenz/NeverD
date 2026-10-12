//===- RegistrationConditionalCallee.cpp - Bound original SEH helpers -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check invocation-specific SEH frame inputs and bounded effects.

#include "RegistrationABIPrivate.h"
#include "RegistrationStateSolver.h"

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ScopeExit.h"

namespace neverd::registration_state {

std::optional<BoundCalleeEffect> proveBoundSEHCallee(const BinaryImage &Image,
                                                     const LowOp &Op,
                                                     const FrameState &Parent,
                                                     size_t &Work) {
  if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != 4)
    return std::nullopt;
  const va_t Target = Op.Inputs[0].Offset;
  const auto *EH = Image.ExceptionMetadata.findFunction(Target);
  if (!EH || !EH->Registration || EH->Cxx || EH->Registration->RealignedFrame ||
      EH->CodeRange.Begin != Target ||
      EH->Personality != ExceptionPersonality::ExceptHandler3)
    return std::nullopt;
  if (!registration_abi::chargeCalleeWork(Work, 5))
    return std::nullopt;
  const auto Call = readImmutableCodeBytes(Image, Op.Addr, 5);
  if (!Call || (*Call)[0] != 0xe8 || Op.Addr > UINT32_MAX - 5 ||
      uint32_t(Op.Addr + 5 + int64_t(readLE<int32_t>(Call->data() + 1))) !=
          Target)
    return std::nullopt;
  const auto SP =
      Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
  if (!SP || *SP < INT32_MIN + 8 || *SP > INT32_MAX - 128)
    return std::nullopt;
  const int64_t Base = int64_t(*SP) - 8;
  ConditionalFrame Inputs;
  Inputs.Work = Work;
  auto AccountWork = llvm::scope_exit([&] { Work = Inputs.Work; });
  for (int32_t Arg = 8; Arg <= 128; Arg += 4) {
    const auto Cell = Parent.Cells.find(*SP + Arg - 8);
    if (Cell == Parent.Cells.end())
      continue;
    const auto &Value = Cell->second;
    if (!Value.MayBeFrame) {
      Inputs.Arguments.emplace(Arg, Value);
      continue;
    }
    if (!Value.Offset || Value.ReturnPC || Value.CallbackAddress ||
        Value.SEHException)
      continue;
    const auto Object = Parent.Cells.find(*Value.Offset);
    const int64_t Offset = int64_t(*Value.Offset) - Base;
    if (Object == Parent.Cells.end() || Object->second.MayBeFrame ||
        *Value.Offset < *SP + 128 || Offset > INT32_MAX - 4 || Offset < 136 ||
        *Value.Offset >= -24 || int64_t(*Value.Offset) + 4 > -24)
      continue;
    Inputs.Arguments.emplace(Arg, FrameValue::frame(int32_t(Offset)));
    Inputs.Objects.push_back({int32_t(Offset), int32_t(Offset + 4)});
    Inputs.ObjectCells.emplace(int32_t(Offset), Object->second);
  }
  if (Inputs.Objects.empty())
    return std::nullopt;
  Inputs.ProveCall = [&](const LowOp &Call, const FrameState &State) {
    return registration_abi::hasConditionalCallFrame(Image, Call, State,
                                                     Inputs.Work);
  };
  Decoder Dec;
  if (!Dec.init(Image))
    return std::nullopt;
  RegistrationCallCalleeIndex Index(Image, &Inputs.Work);
  CFGBuilder Builder;
  Builder.setRegistrationCalleeIndex(&Index);
  const auto Function = Builder.build(Image, Dec, Target, "bound-seh-callee");
  if (!Function.hasCompleteLiftCoverage() ||
      !registration_abi::chargeDecodedCallee(Inputs.Work, Function))
    return std::nullopt;
  for (const auto &Block : Function.Blocks)
    for (const auto &Insn : Block.InstructionBoundaries) {
      if (!registration_abi::chargeCalleeWork(Inputs.Work, 1) ||
          !registration_abi::hasImmutableCalleeInstruction(Image, Insn, Dec))
        return std::nullopt;
      if (Insn.Control == LowInstructionControl::Return) {
        const auto Byte = readImmutableCodeBytes(Image, Insn.Address, 1);
        if (!Byte || (*Byte)[0] != 0xc3 || Insn.Size != 1)
          return std::nullopt;
      }
    }
  const auto Stacks = Index.stackContracts(Function);
  const auto Unwinds = Index.localUnwindContracts(Function);
  const auto Controls = Index.sehControlContracts(Function);
  if (!Stacks || !Unwinds || !Controls)
    return std::nullopt;
  RegistrationStateSolver Solver(Function, 0, 0, nullptr, nullptr, &*Stacks,
                                 &*Unwinds, &*Controls, &Inputs);
  const auto States = Solver.run();
  if (!registration_abi::chargeCalleeWork(Inputs.Work, Solver.workUsed()))
    return std::nullopt;
  if (!States.Complete || !States.CallbackStatesComplete ||
      !States.RegistrationLifetimeComplete || !States.ChainOperationsComplete ||
      !Inputs.Complete)
    return std::nullopt;
  // Filter/finally returns belong to runtime invocations. They cannot prove
  // an ordinary returning call when the helper's own epilogue does not return.
  bool HasParentReturn = false;
  for (const auto &Block : Function.Blocks) {
    const auto State = llvm::find_if(States.Blocks, [&](const auto &Record) {
      return Record.BlockId == Block.Id && Record.Reached &&
             !Record.CallbackOnly;
    });
    if (State == States.Blocks.end())
      continue;
    HasParentReturn |= llvm::any_of(Block.Ops, [](const LowOp &Return) {
      return Return.Opcode == NdOp::RETURN;
    });
  }
  if (!HasParentReturn)
    return std::nullopt;
  BoundCalleeEffect Effect;
  for (const auto &[Begin, End] : Inputs.Reads)
    Effect.Reads.push_back({int32_t(Base + Begin), int32_t(Base + End)});
  for (const auto &[Begin, End] : Inputs.Writes)
    Effect.Writes.push_back({int32_t(Base + Begin), int32_t(Base + End)});
  return Effect;
}
} // namespace neverd::registration_state

namespace neverd {
using namespace registration_state;

RegistrationStateAnalysis analyzeCheckedX86SEHStates(
    const BinaryImage &Image, const LowFunc &Function,
    const std::vector<RegistrationCalleeStackContract> *Stacks,
    const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds,
    const std::vector<RegistrationSEHControlContract> *Controls,
    RegistrationCallCalleeIndex *Index) {
  static thread_local unsigned Depth = 0;
  const bool CheckBoundCalls = Depth < 4;
  ++Depth;
  auto RestoreDepth = llvm::scope_exit([&] { --Depth; });
  BoundCalleeProver Prove = [&](const LowOp &Op, const FrameState &State,
                                size_t &Work) {
    return proveBoundSEHCallee(Image, Op, State, Work);
  };
  RegistrationStateSolver Solver(Function, 0, 0, nullptr, nullptr, Stacks,
                                 LocalUnwinds, Controls, nullptr,
                                 CheckBoundCalls ? &Prove : nullptr);
  auto States = Solver.run();
  if (Index && !Index->chargeStateWork(Solver.workUsed())) {
    States.Complete = States.CallbackStatesComplete =
        States.RegistrationLifetimeComplete = false;
    States.SEHInvocationExitsComplete = States.SEHExceptionAccessesComplete =
        false;
    States.SEHInvocationExits.clear();
    States.SEHExceptionAccesses.clear();
    States.Diagnostics.push_back("SEH callee state proof budget exhausted");
  }
  return States;
}

bool RegistrationCallCalleeIndex::chargeStateWork(size_t Amount) {
  return registration_abi::chargeCalleeWork(Work, Amount);
}

} // namespace neverd
