//===- RegistrationSEHParentFallthrough.cpp - Parent epilogue binding -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationSEHParentFallthrough.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd {

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
      Op.NumInputs != 2 || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].ConstVal != 0 ||
      (Op.Inputs[0].Size != 4 && Op.Inputs[0].Size != 8) ||
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
