//===- KernelLoaderView.cpp - Concrete loader declaration validation ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks declarations before the model allocates or publishes loader state.
//===----------------------------------------------------------------------===//

#include "KernelLoaderView.h"

#include <algorithm>

namespace neverd::emulation::kernel_loader_view {

llvm::Error validate(const DriverLoaderView &View) {
  if (View.Version != Version)
    return llvm::createStringError("driver loader_view: unsupported version");
  if (!View.Flags)
    return llvm::createStringError("driver loader_view: flags is required");
  if (View.Evidence.empty() || View.Evidence.size() > MaxEvidenceSize ||
      !std::all_of(View.Evidence.begin(), View.Evidence.end(),
                   [](unsigned char C) { return C >= ' ' && C <= '~'; }))
    return llvm::createStringError(
        "driver loader_view: evidence must be 1..256 printable ASCII bytes");
  return llvm::Error::success();
}

} // namespace neverd::emulation::kernel_loader_view
