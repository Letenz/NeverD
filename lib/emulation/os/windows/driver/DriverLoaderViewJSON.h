//===- DriverLoaderViewJSON.h - Loader view scenario transport ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Strict JSON transport for a caller-declared concrete loader view.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DRIVERLOADERVIEWJSON_H
#define NEVERD_EMULATION_DRIVERLOADERVIEWJSON_H

#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation::driver_loader_view {
llvm::Expected<DriverLoaderView> parse(const llvm::json::Value &Value);
llvm::json::Object toJSON(const DriverLoaderView &View);
} // namespace neverd::emulation::driver_loader_view

#endif
