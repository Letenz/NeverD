//===- RegistrationStateInitializer.cpp - PE32 construction effects ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind scalar constructor arguments and normal-return initialization.
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::transferInitializer(
    Domain &After, const RegistrationCalleeFrameContract &Callee,
    FrameValue &StackPointer, FrameValue &Object) {
  const auto SP = StackPointer.Offset;
  Object = After.Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride];
  if (!SP || After.Callback || !After.Parent || !Object.Offset ||
      Object.FrameOnlyFromCall || Object.ReturnPC ||
      int64_t(*SP) + Callee.StackPopBytes > INT32_MAX)
    return false;
  const int32_t AfterSP = int32_t(int64_t(*SP) + Callee.StackPopBytes);
  std::vector<RegistrationObjectExtent> Arguments;
  if (Callee.StackPopBytes &&
      !projectFrameObject(After, *SP, SP, {{0, int32_t(Callee.StackPopBytes)}},
                          Arguments, true))
    return false;
  for (const auto *Ranges : {&Callee.ECXReads, &Callee.ECXWrites})
    for (const auto &Range : *Ranges)
      if (!charge(1) || int64_t(*Object.Offset) + Range.Begin < AfterSP)
        return false;
  // May-writes were already invalidated by transferCall. Only bytes written
  // on every normal return can discharge pointer taint or initialize storage.
  for (const auto &Range : Callee.ECXInitialized) {
    const int32_t Begin = int32_t(int64_t(*Object.Offset) + Range.Begin);
    const int32_t End = int32_t(int64_t(*Object.Offset) + Range.End);
    if (!charge(size_t(End - Begin) + After.Frame.cellCount()))
      return false;
    for (int64_t Byte = Begin; Byte < End; ++Byte)
      After.InitializedFrameBytes.insert(int32_t(Byte));
    for (auto It = After.Frame.Cells.begin(); It != After.Frame.Cells.end();)
      if (Begin <= It->first && int64_t(It->first) + 4 <= End)
        It = After.Frame.Cells.erase(It);
      else
        ++It;
  }
  if (!charge(After.InitializedFrameBytes.size() + After.Frame.cellCount()))
    return false;
  After.InitializedFrameBytes.erase(
      After.InitializedFrameBytes.begin(),
      After.InitializedFrameBytes.lower_bound(AfterSP));
  After.Frame.Cells.erase(After.Frame.Cells.begin(),
                          After.Frame.Cells.lower_bound(AfterSP));
  StackPointer = FrameValue::frame(AfterSP);
  return true;
}

} // namespace neverd::registration_state
