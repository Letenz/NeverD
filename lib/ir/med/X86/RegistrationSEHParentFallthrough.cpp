//===- RegistrationSEHParentFallthrough.cpp - Parent epilogue binding -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Binds the source chain removal and the adjoining parent epilogue before and
/// after SSA construction.

#include "RegistrationSEHParentFallthrough.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd {
namespace {

bool isChainHeadAddress(const MedBlock &Block, const MedOp &Store) {
  MedVar Address = Store.Inputs[0];
  if (Address.Size != 4 && Address.Size != 8)
    return false;
  // Before propagation, the lifter materializes a 32-bit segment offset and
  // may zero-extend it to the memory address width. Bind only these adjacent
  // instruction-local definitions, never a register or another instruction.
  const MedOp *Next = &Store;
  for (unsigned Count = 0; Count != 2 && !Address.isConst(); ++Count) {
    if (Address.Kind != MedVar::Temp || Next == Block.Ops.data())
      return false;
    const auto &Def = *(Next - 1);
    if (Def.Dead || Def.NumInputs != 1 || Def.Addr != Store.Addr ||
        Def.OriginSeq < 0 || Def.OriginSeq + 1 != Next->OriginSeq ||
        Def.Output != Address ||
        Def.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        Def.MemoryOrdering != NdMemoryOrdering::None ||
        Def.RegistrationRoot != MedOp::RegistrationRootKind::None ||
        Def.ExactArguments || Def.SourceCallHint)
      return false;
    const bool Copy =
        Def.Opcode == NdOp::COPY && Def.Inputs[0].Size == Address.Size;
    const bool Extend = Def.Opcode == NdOp::INT_ZEXT && Address.Size == 8 &&
                        Def.Inputs[0].Size == 4;
    if (!Copy && !Extend)
      return false;
    Address = Def.Inputs[0];
    Next = &Def;
  }
  return Address.isConst() && Address.ConstVal == 0;
}

} // namespace

bool isRegistrationSEHParentFallthrough(
    const MedBlock &Block, const MedOp &Op, const MedBlock &Target,
    const RegistrationStateAnalysis &States,
    const RegistrationSEHInvocationExit &Exit, size_t &Work) {
  for (const size_t Count :
       {States.ChainAccesses.size(), States.SEHControls.size()}) {
    if (Work > limits::kMaxRegistrationEHStateWork ||
        Count > limits::kMaxRegistrationEHStateWork - Work)
      return false;
    Work += Count;
  }
  if (Exit.Exit != RegistrationSEHInvocationExit::Kind::ParentFallthrough ||
      !States.ChainOperationsComplete || Op.Dead || Op.OriginSeq < 0 ||
      Op.ExactArguments || Op.SourceCallHint || Op.Opcode != NdOp::STORE ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::X86FS ||
      Op.MemoryOrdering != NdMemoryOrdering::None || Op.Output.Size ||
      Op.NumInputs != 2 || !isChainHeadAddress(Block, Op) ||
      Op.Inputs[1].Size != 4 || Op.Inputs[1].isConst() ||
      Exit.EndAddress != Exit.TargetVA || Block.EndAddr != Target.StartAddr ||
      !Target.Succs.empty() || Target.Ops.empty())
    return false;
  const auto &Return = Target.Ops.back();
  if (Return.Dead || Return.OriginSeq < 0 || Return.Opcode != NdOp::RETURN ||
      Return.Addr != Target.EndAddr - 1 ||
      Return.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Return.MemoryOrdering != NdMemoryOrdering::None)
    return false;
  const auto Removes = llvm::count_if(States.ChainAccesses, [&](const auto &A) {
    return A.AccessKind == RegistrationChainAccess::Kind::Remove &&
           A.Address == Exit.Address && A.OpSeq == Exit.OpSeq &&
           A.EndAddress == Exit.EndAddress;
  });
  const auto Epilogues = llvm::count_if(States.SEHControls, [&](const auto &C) {
    return C.Control == RegistrationSEHControlContract::Kind::ParentEpilogue &&
           C.Address == Target.StartAddr && C.EndAddress == Target.EndAddr &&
           C.Target == Return.Addr;
  });
  return Removes == 1 && Epilogues == 1;
}

} // namespace neverd
