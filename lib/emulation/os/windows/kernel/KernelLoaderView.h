//===- KernelLoaderView.h - Declared concrete loader layout ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validation and byte layout of the optional concrete loader record.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELLOADERVIEW_H
#define NEVERD_EMULATION_KERNELLOADERVIEW_H

#include "neverd/emulation/DriverSession.h"
#include "neverd/emulation/Limits.h"

namespace neverd::emulation::kernel_loader_view {

constexpr uint32_t Version = 1;
constexpr uint64_t RecordSize = 160;
struct Field {
  uint64_t Offset;
  unsigned Size;
  constexpr bool contains(uint64_t Byte) const {
    return Byte >= Offset && Byte - Offset < Size;
  }
};
constexpr Field DllBase{48, 8};
constexpr Field EntryPoint{56, 8};
constexpr Field SizeOfImage{64, 4};
constexpr Field Flags{104, 4};

constexpr bool readable(uint64_t Byte) {
  return DllBase.contains(Byte) || EntryPoint.contains(Byte) ||
         SizeOfImage.contains(Byte) || Flags.contains(Byte);
}

llvm::Error validate(const DriverLoaderView &View);

} // namespace neverd::emulation::kernel_loader_view

// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::emulation::kernel_loader_view {
inline constexpr size_t MaxEvidenceSize =
    ::neverd::limits::emulation::kKernelLoaderViewMaxEvidenceSize;
}

#endif
