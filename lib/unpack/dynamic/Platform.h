//===- Platform.h - Instruction set and guest system selection --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_DYNAMIC_PLATFORM_H
#define NEVERD_UNPACK_DYNAMIC_PLATFORM_H

#include "../core/Image.h"

#include "neverd/emulation/ProcessObserver.h"

namespace neverd::unpack {
namespace text {
#define NEVERD_UNPACK_OBSERVATION_TEXT(Name, Text)                             \
  inline constexpr char Name[] = Text;
#include "Observation.def"
#undef NEVERD_UNPACK_OBSERVATION_TEXT
} // namespace text

/// What judging a transfer needs to know about an instruction set.
struct ArchitectureTraits {
  emulation::CPURegister StackPointer;
  /// The longest instruction, in bytes.
  uint64_t InstructionWindow;
};

/// The traits of \p Architecture, or an error that names it.
llvm::Expected<ArchitectureTraits>
architectureTraits(emulation::GuestArchitecture Architecture);
/// The guest process profile that executes \p Image, or an error that names
/// its container and instruction set.
llvm::Expected<emulation::ProcessProfile>
processProfile(const InputImage &Image);
} // namespace neverd::unpack
#endif
