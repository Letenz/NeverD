//===- MedLLVMRegistrationCxxObjectCall.cpp - PE32 receiver plans ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMRegistrationCxxCall.h"

#include "neverd/Limits.h"
#include "neverd/ir/RegistrationState.h"

namespace neverd::med_llvm_eh {
bool hasLiveRegistrationCxxRuntimeReceiver(
    const RegistrationStateAnalysis &States,
    const RegistrationBlockState &Block,
    const RegistrationCalleeFrameContract &Contract,
    const RegistrationCallFrameEffect &Effect, size_t &Work) {
  if (!Effect.ECXRuntimeObject)
    return true;
  if (Effect.ECXFrameOffset || !Block.CallbackOnly ||
      Block.CxxCatchStacks.empty() || !States.CxxCatchObjectsComplete ||
      Contract.CalleeKind != RegistrationCalleeFrameContract::Kind::Leaf)
    return false;
  const auto &Receiver = *Effect.ECXRuntimeObject;
  const RegistrationCxxCatchObject *Object = nullptr;
  for (const auto &Candidate : States.CxxCatchObjects) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return false;
    if (Candidate.TryIndex != Receiver.TryIndex ||
        Candidate.CatchIndex != Receiver.CatchIndex)
      continue;
    if (Object)
      return false;
    Object = &Candidate;
  }
  if (!Object || !Object->Reference || Receiver.Offset < 0 ||
      uint32_t(Receiver.Offset) > Object->ObjectSize)
    return false;
  for (const auto &Stack : Block.CxxCatchStacks) {
    bool Live = false;
    for (const auto &Identity : Stack) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      Live |=
          Identity == std::make_pair(Receiver.TryIndex, Receiver.CatchIndex);
    }
    if (!Live)
      return false;
  }
  for (const auto *Extents : {&Contract.ECXReads, &Contract.ECXWrites})
    for (const auto &Extent : *Extents) {
      const int64_t Begin = int64_t(Receiver.Offset) + Extent.Begin;
      const int64_t End = int64_t(Receiver.Offset) + Extent.End;
      if (++Work > limits::kMaxRegistrationEHStateWork || Begin < 0 ||
          End <= Begin || uint64_t(End) > Object->ObjectSize)
        return false;
    }
  return true;
}
} // namespace neverd::med_llvm_eh
