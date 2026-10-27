//===- KernelPhysicalInventory.h - Declared simulated RAM inventory -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declared simulated RAM inventory.
///
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_KERNELPHYSICALINVENTORY_H
#define NEVERD_EMULATION_KERNELPHYSICALINVENTORY_H

#include "neverd/emulation/DriverSession.h"
#include "neverd/emulation/Limits.h"

namespace neverd::emulation::kernel_physical_inventory {
constexpr uint32_t Version = 1;
llvm::Error validate(const DriverPhysicalMemoryInventory &Inventory);
} // namespace neverd::emulation::kernel_physical_inventory
// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::emulation::kernel_physical_inventory {
inline constexpr size_t MaxEvidenceSize =
    ::neverd::limits::emulation::kKernelPhysicalInventoryMaxEvidenceSize;
}

#endif
