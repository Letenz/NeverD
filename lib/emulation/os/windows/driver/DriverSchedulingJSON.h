//===- DriverSchedulingJSON.h - Driver schedule validation and wire form -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_DRIVERSCHEDULINGJSON_H
#define NEVERD_EMULATION_DRIVERSCHEDULINGJSON_H

#include "neverd/emulation/DriverScheduling.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation {
llvm::Error validateDriverScheduling(const DriverScheduling &Policy);
llvm::Expected<DriverScheduling>
parseDriverScheduling(const llvm::json::Value &Value);
llvm::json::Object driverSchedulingJSON(const DriverScheduling &Policy);
} // namespace neverd::emulation
#endif
