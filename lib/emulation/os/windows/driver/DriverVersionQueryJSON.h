//===- DriverVersionQueryJSON.h - Version query declaration transport
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Serialization of bounded version query environment declarations.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DRIVERVERSIONQUERYJSON_H
#define NEVERD_EMULATION_DRIVERVERSIONQUERYJSON_H
#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation::driver_version_query {
llvm::Expected<DriverVersionQuery> parse(const llvm::json::Value &Value);
llvm::json::Object toJSON(const DriverVersionQuery &Query);
} // namespace neverd::emulation::driver_version_query
#endif
