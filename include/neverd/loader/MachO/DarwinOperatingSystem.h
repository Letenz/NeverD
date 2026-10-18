//===- DarwinOperatingSystem.h - A Mach-O image's platform ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_MACHO_DARWINOPERATINGSYSTEM_H
#define NEVERD_LOADER_MACHO_DARWINOPERATINGSYSTEM_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/BinaryFormat/MachO.h"

#include <optional>

namespace neverd {

/// The Apple platforms whose headers declare what a program's system
/// routines take: libSystem's for macOS, and for iOS, which leaves out the
/// legacy interfaces macOS keeps.
enum class DarwinPlatform : uint8_t { MacOS, IOS };

/// The platform \p Image is built for, as its one build version command
/// names it: macOS, or iOS and its simulator.  None for an image that names
/// no platform or more than one, for another platform (Mac Catalyst, tvOS,
/// watchOS and the others, whose headers differ again), and for an image
/// that is not Mach-O.
inline std::optional<DarwinPlatform> darwinPlatform(const BinaryImage &Image) {
  if (!Image.isMachO() || Image.DynInfo.MachOPlatforms.size() != 1)
    return std::nullopt;
  switch (Image.DynInfo.MachOPlatforms.front()) {
  case llvm::MachO::PLATFORM_MACOS:
    return DarwinPlatform::MacOS;
  case llvm::MachO::PLATFORM_IOS:
  case llvm::MachO::PLATFORM_IOSSIMULATOR:
    return DarwinPlatform::IOS;
  default:
    return std::nullopt;
  }
}

} // namespace neverd

#endif // NEVERD_LOADER_MACHO_DARWINOPERATINGSYSTEM_H
