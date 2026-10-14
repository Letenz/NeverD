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

namespace neverd::emulation::kernel_physical_inventory {
constexpr uint32_t Version = 1;
constexpr size_t MaxEvidenceSize = 256;
llvm::Error validate(const DriverPhysicalMemoryInventory &Inventory);
} // namespace neverd::emulation::kernel_physical_inventory
#endif
