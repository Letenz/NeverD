//===- RegistrationStateSEHException.cpp - SEH runtime access receipts ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Binds exception-field accesses to exact source instruction occurrences.

#include "RegistrationStateSolver.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::recordSEHExceptionMemory(
    size_t I, Domain &After, const LowOp &Op, const FrameTransfer &Transfer,
    BlockFacts &Facts) {
  const auto Memory = lowMemoryOperands(Op);
  if (KnownCxx || !Memory.Address || !charge(1))
    return false;
  const auto Address = Transfer.read(*Memory.Address);
  const auto Value = Transfer.evaluate(Op, After.Installed);
  const bool Slot = Address.Offset && Value.SEHException &&
                    Address.Offset == int64_t(*Chain.RegistrationOffset) - 4;
  const bool RuntimeAddress = Address.SEHException.has_value();
  std::optional<RegistrationSEHExceptionAccess> Access;
  if (Slot || RuntimeAddress) {
    const auto Boundary = Boundaries.find(Op.Addr);
    bool Valid =
        !EH4 && After.Callback && !After.Parent &&
        After.Activation.SearchFilter && !After.Activation.OrdinaryCall &&
        After.Activation.Entry == After.Frame.SEHFilterEntry &&
        !After.Unknown && !Facts.Invalid && Memory.Complete &&
        Op.Opcode == NdOp::LOAD && Op.Output.Size == 4 && Op.Seq >= 0 &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        Op.MemoryOrdering == NdMemoryOrdering::None &&
        Boundary != Boundaries.end() &&
        Boundary->second.first == Function.Blocks[I].Id;
    if (Valid) {
      const auto &Insn = Boundary->second.second;
      const auto &Block = Function.Blocks[I];
      const size_t OpIndex = &Op - Block.Ops.data();
      Valid =
          Insn.FirstOp <= Block.Ops.size() &&
          Insn.OpCount <= Block.Ops.size() - Insn.FirstOp &&
          OpIndex >= Insn.FirstOp && OpIndex - Insn.FirstOp < Insn.OpCount &&
          Insn.Mode == InstructionMode::Default &&
          !hasLowInstructionControlFlag(
              Insn.ControlFlags, LowInstructionControlFlag::Conditional) &&
          !hasLowInstructionControlFlag(
              Insn.ControlFlags, LowInstructionControlFlag::InstructionGuard);
    }
    RegistrationSEHExceptionAccess::Kind Field;
    if (Slot) {
      Valid &=
          *Chain.RegistrationOffset >= INT32_MIN + 4 &&
          Address.Offset == int64_t(*Chain.RegistrationOffset) - 4 &&
          Value.SEHException->Filter == After.Frame.SEHFilterEntry &&
          Value.SEHException->Storage == SEHExceptionAddress::Kind::Pointers &&
          Value.SEHException->Offset == 0;
      Field = RegistrationSEHExceptionAccess::Kind::PointersSlot;
    } else {
      Valid &=
          After.Frame.sehExceptionLoad(*Address.SEHException, Memory.AccessSize)
              .has_value();
      Field =
          Address.SEHException->Storage == SEHExceptionAddress::Kind::Pointers
              ? RegistrationSEHExceptionAccess::Kind::RecordPointer
          : Address.SEHException->Offset == 0
              ? RegistrationSEHExceptionAccess::Kind::ExceptionCode
              : RegistrationSEHExceptionAccess::Kind::ExceptionFlags;
    }
    if (Valid)
      Access = {Field, Op.Addr, Op.Seq, *After.Frame.SEHFilterEntry};
    else {
      Result.SEHExceptionAccessesComplete = CompleteImageReads = false;
      Facts.Invalid = After.Unknown = true;
      After.Levels.clear();
    }
  }
  const auto [It, New] =
      SEHExceptionAccesses.emplace(std::make_pair(Op.Addr, Op.Seq), Access);
  if (!New && It->second != Access) {
    Result.SEHExceptionAccessesComplete = false;
    It->second.reset();
  }
  return Access.has_value();
}

} // namespace neverd::registration_state
