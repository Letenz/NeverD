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
  const auto &Address = Store.Inputs[0];
  if (Address.Size != 4 && Address.Size != 8)
    return false;
  if (Address.isConst())
    return Address.ConstVal == 0;
  // Before propagation, the x86 lifter materializes the segment offset in an
  // instruction-local temporary. Bind that exact producer, never a register
  // with an equal value or a definition borrowed from another instruction.
  if (Address.Kind != MedVar::Temp || &Store == Block.Ops.data())
    return false;
  const auto &Copy = *(&Store - 1);
  return !Copy.Dead && Copy.Opcode == NdOp::COPY && Copy.NumInputs == 1 &&
         Copy.Addr == Store.Addr && Copy.OriginSeq >= 0 &&
         Copy.OriginSeq + 1 == Store.OriginSeq && Copy.Output == Address &&
         Copy.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
         Copy.MemoryOrdering == NdMemoryOrdering::None &&
         Copy.RegistrationRoot == MedOp::RegistrationRootKind::None &&
         !Copy.ExactArguments && !Copy.SourceCallHint &&
         Copy.Inputs[0].isConst() && Copy.Inputs[0].ConstVal == 0 &&
         Copy.Inputs[0].Size == Address.Size;
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
