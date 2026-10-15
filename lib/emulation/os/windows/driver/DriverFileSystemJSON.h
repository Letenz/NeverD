//===- DriverFileSystemJSON.h - Declared filesystem transport -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Strict scenario input and separate observed filesystem state transport.

#ifndef NEVERD_EMULATION_DRIVERFILESYSTEMJSON_H
#define NEVERD_EMULATION_DRIVERFILESYSTEMJSON_H

#include "neverd/emulation/DriverFileSystem.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation::driver_filesystem {
llvm::Expected<std::vector<DriverFileSystemVolume>>
parse(const llvm::json::Value &Value);
llvm::json::Value configurationJSON(
    const std::optional<std::vector<DriverFileSystemVolume>> &Volumes);
llvm::json::Value stateJSON(
    const std::optional<std::vector<DriverFileSystemVolumeState>> &Volumes);
} // namespace neverd::emulation::driver_filesystem

#endif
