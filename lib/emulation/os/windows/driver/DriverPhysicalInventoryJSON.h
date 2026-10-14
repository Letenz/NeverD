//===- DriverPhysicalInventoryJSON.h - Inventory declaration transport ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Inventory declaration transport.
///
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_DRIVERPHYSICALINVENTORYJSON_H
#define NEVERD_EMULATION_DRIVERPHYSICALINVENTORYJSON_H
#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation::driver_physical_inventory {
llvm::Expected<DriverPhysicalMemoryInventory>
parse(const llvm::json::Value &Value);
llvm::json::Object toJSON(const DriverPhysicalMemoryInventory &Inventory);
} // namespace neverd::emulation::driver_physical_inventory
#endif
