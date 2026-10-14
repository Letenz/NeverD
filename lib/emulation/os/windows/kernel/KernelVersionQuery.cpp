//===- KernelVersionQuery.cpp - Concrete query declaration validation
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validate explicit concrete version query declarations.
///
//===----------------------------------------------------------------------===//

#include "KernelVersionQuery.h"

#include <algorithm>

namespace neverd::emulation::kernel_version_query {
llvm::Error validate(const DriverVersionQuery &Query) {
  if (Query.Version != Version)
    return llvm::createStringError("driver version_query: unsupported version");
  if (!Query.BuildNumber || *Query.BuildNumber > MaxBuild)
    return llvm::createStringError(
        "driver version_query: build_number must be supplied in 0..65535");
  if (Query.Evidence.empty() || Query.Evidence.size() > MaxEvidenceSize ||
      !std::all_of(Query.Evidence.begin(), Query.Evidence.end(),
                   [](unsigned char C) { return C >= ' ' && C <= '~'; }))
    return llvm::createStringError(
        "driver version_query: evidence must be 1..256 printable ASCII bytes");
  return llvm::Error::success();
}
} // namespace neverd::emulation::kernel_version_query
