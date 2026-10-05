#ifndef NEVERD_LOADER_SWIFT_SWIFTONCECALLBACKABI_H
#define NEVERD_LOADER_SWIFT_SWIFTONCECALLBACKABI_H

#include "neverd/ir/SourceABI.h"

#include <stdexcept>

namespace neverd {
// Runtime/Once.h declares a C callback returning void and taking one context
// pointer. This is declaration geometry only; the source once owner must still
// authenticate a current callback reference and its context independence.
inline SourceFunctionTypeHint swiftOnceCallbackSourceABI(Arch Architecture) {
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
  Hint.ReturnType = NdType::makeVoid();
  Hint.Parameters = {{"once_context", NdType::makePtr(NdType::makeVoid())}};
  std::string Error;
  if (!assignDarwinScalarSourceABI(Hint, Architecture, Error))
    throw std::invalid_argument(Error);
  return Hint;
}

inline bool isSwiftOnceCallbackSourceABI(const SourceFunctionTypeHint &Hint) {
  return (Hint.Architecture == Arch::AArch64 ||
          Hint.Architecture == Arch::X64) &&
         Hint.Origin == SourceFunctionTypeHint::OriginKind::SwiftRuntime &&
         equalSourceABIs(Hint, swiftOnceCallbackSourceABI(Hint.Architecture));
}
} // namespace neverd
#endif
