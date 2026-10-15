//===- DriverVaShadowQueryJSON.h - Mitigation query transport ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Mitigation query transport.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DRIVERVASHADOWQUERYJSON_H
#define NEVERD_EMULATION_DRIVERVASHADOWQUERYJSON_H
#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation::driver_va_shadow_query {
llvm::Expected<DriverKernelVaShadowQuery> parse(const llvm::json::Value &Value);
llvm::json::Object toJSON(const DriverKernelVaShadowQuery &Query);
} // namespace neverd::emulation::driver_va_shadow_query
#endif
