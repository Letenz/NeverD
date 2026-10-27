//===- MachOPlatform.h - A Mach-O image's build platform --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The platform (llvm::MachO::PlatformType) a Mach-O image's load commands
/// say it is built for, which the analysis image and the emulator's
/// execution image both read.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_MACHO_MACHOPLATFORM_H
#define NEVERD_LOADER_MACHO_MACHOPLATFORM_H

#include "neverd/Common.h"

#include "llvm/Object/MachO.h"

#include <optional>

namespace neverd {

/// The platform load command \p Command of \p Obj names: LC_BUILD_VERSION's
/// own, or the one an older LC_VERSION_MIN_ command implies, where a legacy
/// iOS command on x64 denotes a simulator binary.  None for another command.
inline std::optional<uint32_t> machOCommandPlatform(
    const llvm::object::MachOObjectFile &Obj,
    const llvm::object::MachOObjectFile::LoadCommandInfo &Command,
    Arch Target) {
  switch (Command.C.cmd) {
  case llvm::MachO::LC_BUILD_VERSION:
    if (Command.C.cmdsize < sizeof(llvm::MachO::build_version_command))
      return std::nullopt;
    return Obj.getBuildVersionLoadCommand(Command).platform;
  case llvm::MachO::LC_VERSION_MIN_MACOSX:
    return llvm::MachO::PLATFORM_MACOS;
  case llvm::MachO::LC_VERSION_MIN_IPHONEOS:
    return Target == Arch::X64 ? llvm::MachO::PLATFORM_IOSSIMULATOR
                               : llvm::MachO::PLATFORM_IOS;
  default:
    return std::nullopt;
  }
}

} // namespace neverd

#endif // NEVERD_LOADER_MACHO_MACHOPLATFORM_H
