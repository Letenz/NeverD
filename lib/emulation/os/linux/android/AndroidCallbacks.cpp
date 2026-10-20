//===- AndroidCallbacks.cpp - Returning guest callback continuations -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Resume the suspended Bionic operation after an actual guest return.

#include "AndroidInternal.h"

#include <type_traits>

namespace neverd::emulation::android_model {
BionicResult Bionic::finishCallback(const GuestCallback &Callback) {
  return std::visit(
      [&](const auto &Continuation) -> BionicResult {
        using T = std::decay_t<decltype(Continuation)>;
        if constexpr (std::is_same_v<T, OnceCallback>) {
          if (auto E = finishOnce(Continuation))
            return std::move(E);
          return value(0);
        } else if constexpr (std::is_same_v<T, FinalizeCallback>) {
          return finalize(Continuation.DSO);
        } else if constexpr (std::is_same_v<T, PropertyCallback>) {
          return nextProperty(Continuation);
        } else if constexpr (std::is_same_v<T, JNINativeCallback>) {
          return finishNativeJNI(Continuation);
        } else if constexpr (std::is_same_v<T, JNIOnLoadCallback>) {
          return finishJavaLibrary(Continuation);
        } else {
          static_assert(std::is_same_v<T, PropertyReadCallback>);
          // The read-callback ABI is void; x0 is unspecified on return.
          return value(0);
        }
      },
      Callback.Continuation);
}
} // namespace neverd::emulation::android_model
