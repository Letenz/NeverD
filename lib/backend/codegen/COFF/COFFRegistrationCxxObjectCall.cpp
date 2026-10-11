//===- COFFRegistrationCxxObjectCall.cpp - Runtime object borrows ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationCxxIRProof.h"
#include "COFFRegistrationFrameProof.h"

#include "llvm/IR/Instructions.h"

namespace neverd::coff_registration {
llvm::Error bindCxxRuntimeObjectBorrow(const CxxIRControlProof &Proof,
                                       const llvm::CallBase &Call,
                                       const CxxIRCall &Checked,
                                       RegistrationCxxFrameContract &Contract) {
  if (!Checked.RuntimeObject || Checked.ObjectFrameOffset || Checked.Cleanup ||
      Checked.Contract.CalleeKind !=
          RegistrationCalleeFrameContract::Kind::Leaf)
    return rejectIR("C++ runtime member borrow has no ordinary source call");
  const auto &Object = *Checked.RuntimeObject;
  const auto Found = Proof.Catches.find({Object.TryIndex, Object.CatchIndex});
  if (Found == Proof.Catches.end() || !Found->second.Home ||
      !Found->second.Home->Reference || Object.Offset < 0 ||
      uint32_t(Object.Offset) > Found->second.Home->ObjectSize)
    return rejectIR("C++ runtime member borrow has no reference object owner");
  RegistrationFrameBorrow Borrow{Object.Offset, Checked.Contract.ECXReads,
                                 Checked.Contract.ECXWrites};
  Borrow.Root = Found->second.Pad;
  if (!Contract.Borrows.emplace(&Call, std::move(Borrow)).second)
    return rejectIR("C++ runtime member borrow has duplicate source ownership");
  return llvm::Error::success();
}
} // namespace neverd::coff_registration
