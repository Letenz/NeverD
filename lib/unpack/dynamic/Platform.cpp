//===- Platform.cpp - Instruction set and guest system selection ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "Platform.h"

namespace neverd::unpack {
using emulation::GuestArchitecture;

llvm::Expected<ArchitectureTraits>
architectureTraits(GuestArchitecture Architecture) {
#define NEVERD_UNPACK_ARCHITECTURE(Name, StackPointer, Window)                 \
  if (Architecture == GuestArchitecture::Name)                                 \
    return ArchitectureTraits{emulation::CPURegister::StackPointer, Window};
#include "Observation.def"
#undef NEVERD_UNPACK_ARCHITECTURE
  return failure(llvm::Twine(text::NoArchitecture) +
                 architectureName(Architecture));
}

llvm::Expected<emulation::ProcessProfile>
processProfile(const InputImage &Image) {
#define NEVERD_UNPACK_PLATFORM(Format, Architecture, Profile)                  \
  if (Image.format() == FormatKind::Format &&                                  \
      Image.architecture() == GuestArchitecture::Architecture)                 \
    return emulation::ProcessProfile::Profile;
#include "Observation.def"
#undef NEVERD_UNPACK_PLATFORM
  return failure(llvm::Twine(text::NoPlatform) +
                 formatKindName(Image.format()) + text::PlatformSeparator +
                 architectureName(Image.architecture()));
}
} // namespace neverd::unpack
