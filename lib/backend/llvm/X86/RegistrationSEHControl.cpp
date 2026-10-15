//===- RegistrationSEHControl.cpp - Source finally returns ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/X86RegistrationSEHControl.h"
#include "neverd/ir/med/X86RegistrationSEHCall.h"

namespace neverd {

std::optional<std::map<int, va_t>>
getX86RegistrationSEHReturnBlocks(const MedFunc &Function) {
  auto Returns = getRegistrationSEHReturns(Function);
  if (!Returns)
    return std::nullopt;
  return std::move(Returns->ReturnBlocks);
}

} // namespace neverd
