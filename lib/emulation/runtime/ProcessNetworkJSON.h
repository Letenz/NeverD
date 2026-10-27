//===- ProcessNetworkJSON.h - Offline network wire input ---------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded offline network contracts and their semantic checks.

#ifndef NEVERD_EMULATION_PROCESSNETWORKJSON_H
#define NEVERD_EMULATION_PROCESSNETWORKJSON_H
#include "neverd/emulation/AndroidResolverOptions.h"
#include "neverd/emulation/LinuxNetworkOptions.h"

#include "llvm/Support/JSON.h"
namespace neverd::emulation {
llvm::Expected<LinuxNetworkOptions>
linuxNetworkOptionsFromJSON(const llvm::json::Value &Value);
llvm::Expected<std::vector<AndroidResolverQuery>>
androidResolverQueriesFromJSON(const llvm::json::Value &Value);
} // namespace neverd::emulation
#endif
