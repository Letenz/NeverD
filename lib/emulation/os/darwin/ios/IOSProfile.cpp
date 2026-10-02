//===- IOSProfile.cpp - Explicit iOS device and simulator guests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../DarwinProcess.h"

#include "llvm/BinaryFormat/MachO.h"
namespace neverd::emulation::darwin_model {
ProfileSpec iOSProfile(bool Simulator) {
  return Simulator ? ProfileSpec{ProcessProfile::IOSSimulatorMachO64,
                                 llvm::MachO::PLATFORM_IOSSIMULATOR, true}
                   : ProfileSpec{ProcessProfile::IOSMachO64,
                                 llvm::MachO::PLATFORM_IOS, false};
}
} // namespace neverd::emulation::darwin_model
