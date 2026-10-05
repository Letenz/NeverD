//===- AndroidFinalizers.cpp - Guest C++ destruction callbacks -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

namespace neverd::emulation::android_model {
BionicResult Bionic::registerExit(const NativeCallEvent &Call) {
  // A finite model limit, not a claim about Bionic's allocation capacity.
  // Null callbacks are inert in API 28 but still consume registration space.
  constexpr size_t MaxExitCallbacks = 4096;
  if (ExitCallbacks.size() == MaxExitCallbacks)
    return failure(diagnostic::ExitCallbackLimit);
  const auto &A = Call.Arguments;
  ExitCallbacks.push_back({A[0], A[1], A[2]});
  return std::optional<BionicValue>(uint64_t(0));
}

BionicResult Bionic::finalize(uint64_t DSO) {
  // Rescan after each returning callback: guest code may register a newer
  // handler or recursively finalize either this DSO or the entire registry.
  for (size_t I = ExitCallbacks.size(); I != 0; --I) {
    const auto Callback = ExitCallbacks[I - 1];
    if (!Callback.Entry || (DSO && Callback.DSO != DSO))
      continue;
    if (Callback.Entry % 4)
      return failure(diagnostic::ExitCallbackAlignment);
    if (auto E = access(Callback.Entry, 4, Execute))
      return std::move(E);
    // Retire before entering guest code so a recursive finalize cannot invoke
    // this registration again. Do not retain an iterator across that code.
    ExitCallbacks.erase(ExitCallbacks.begin() + I - 1);
    return std::optional<BionicValue>(GuestCallback{
        Callback.Entry, Callback.Argument, FinalizeCallback{DSO}});
  }
  if (!DSO)
    ExitCallbacks.clear();
  // The ABI leaves x0 unspecified for this void call.
  return std::optional<BionicValue>(uint64_t(0));
}

BionicResult Bionic::finishCallback(const GuestCallback &Callback) {
  if (const auto *Once = std::get_if<OnceCallback>(&Callback.Continuation)) {
    if (auto E = finishOnce(*Once))
      return std::move(E);
    return std::optional<BionicValue>(uint64_t(0));
  }
  return finalize(std::get<FinalizeCallback>(Callback.Continuation).DSO);
}
BionicResult Bionic::finalizeCall(const NativeCallEvent &Call) {
  return finalize(Call.Arguments[0]);
}

} // namespace neverd::emulation::android_model
