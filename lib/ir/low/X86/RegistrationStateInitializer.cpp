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
  Object = After.Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride];
  if (!Object.Offset || Object.FrameOnlyFromCall || Object.ReturnPC)
    return false;
  // Catch arguments live in the active callback invocation; the destination
  // remains in the independently proved parent frame. Releasing arguments
  // must never retire parent object storage.
  if (StackPointer.CallbackAddress) {
    const auto Stack = *StackPointer.CallbackAddress;
    const int64_t End = int64_t(Stack.Offset) + Callee.StackPopBytes;
    if (!KnownCxx || After.Parent || !After.Callback || After.OtherCallback ||
        After.CxxCatchStacks.size() != 1 ||
        After.CxxCatchStacks.begin()->empty() ||
        After.Frame.CallbackEntry != Stack.Entry || End > 0 ||
        !charge(Callee.StackPopBytes + After.Frame.cellCount()))
      return false;
    if (Callee.StackPopBytes &&
        !After.Frame.callbackMemoryIsPrivate(
            Stack, uint16_t(Callee.StackPopBytes), true))
      return false;
    for (const auto &[Offset, Value] : After.Frame.CallbackCells)
      if (Value.MayBeFrame && int64_t(Offset) < End &&
          Stack.Offset < int64_t(Offset) + 4)
        return false;
    After.Frame.CallbackCells.erase(
        After.Frame.CallbackCells.begin(),
        After.Frame.CallbackCells.lower_bound(int32_t(End)));
    After.Frame.InitializedCallbackBytes.erase(
        After.Frame.InitializedCallbackBytes.begin(),
        After.Frame.InitializedCallbackBytes.lower_bound(int32_t(End)));
    StackPointer = FrameValue::callbackFrame(Stack.Entry, int32_t(End));
  } else {
    const auto SP = StackPointer.Offset;
    if (!SP || After.Callback || !After.Parent ||
        int64_t(*SP) + Callee.StackPopBytes > INT32_MAX)
      return false;
    const int32_t AfterSP = int32_t(int64_t(*SP) + Callee.StackPopBytes);
    std::vector<RegistrationObjectExtent> Arguments;
    if (Callee.StackPopBytes &&
        !projectFrameObject(After, *SP, SP,
                            {{0, int32_t(Callee.StackPopBytes)}}, Arguments,
                            true))
      return false;
    for (const auto *Ranges : {&Callee.ECXReads, &Callee.ECXWrites})
      for (const auto &Range : *Ranges)
        if (!charge(1) || int64_t(*Object.Offset) + Range.Begin < AfterSP)
          return false;
    StackPointer = FrameValue::frame(AfterSP);
  }
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
  if (const auto AfterSP = StackPointer.Offset) {
    After.InitializedFrameBytes.erase(
        After.InitializedFrameBytes.begin(),
        After.InitializedFrameBytes.lower_bound(*AfterSP));
    After.Frame.Cells.erase(After.Frame.Cells.begin(),
                            After.Frame.Cells.lower_bound(*AfterSP));
  }
  return true;
}

} // namespace neverd::registration_state
