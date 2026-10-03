//===- MacOSProfile.cpp - Explicit macOS guest platform ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../process/DarwinProcess.h"

#include "llvm/BinaryFormat/MachO.h"
namespace neverd::emulation::darwin_model {
ProfileSpec macOSProfile() {
  return {ProcessProfile::MacOSMachO64, llvm::MachO::PLATFORM_MACOS, true};
}
} // namespace neverd::emulation::darwin_model
