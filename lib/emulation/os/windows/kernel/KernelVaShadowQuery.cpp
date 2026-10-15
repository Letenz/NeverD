//===- KernelVaShadowQuery.cpp - Declared mitigation query validation ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declared mitigation query validation.
///
//===----------------------------------------------------------------------===//

#include "KernelVaShadowQuery.h"

#include <algorithm>

namespace neverd::emulation::kernel_va_shadow_query {
llvm::Error validate(const DriverKernelVaShadowQuery &Query) {
  if (Query.Version != Version)
    return llvm::createStringError(
        "driver kernel_va_shadow_query: unsupported version");
  if (!Query.Flags || (*Query.Flags & ~ValidFlags))
    return llvm::createStringError("driver kernel_va_shadow_query: flags must "
                                   "be supplied with reserved bits zero");
  if (Query.Evidence.empty() || Query.Evidence.size() > MaxEvidenceSize ||
      !std::all_of(Query.Evidence.begin(), Query.Evidence.end(),
                   [](unsigned char C) { return C >= ' ' && C <= '~'; }))
    return llvm::createStringError("driver kernel_va_shadow_query: evidence "
                                   "must be 1..256 printable ASCII bytes");
  return llvm::Error::success();
}
} // namespace neverd::emulation::kernel_va_shadow_query
