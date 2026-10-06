//===- ProcessDarwinSystemJSON.h - System inputs -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSDARWINSYSTEMJSON_H
#define NEVERD_EMULATION_PROCESSDARWINSYSTEMJSON_H

#include "neverd/emulation/DarwinSystemOptions.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

namespace neverd::emulation {
llvm::Expected<DarwinSystemOptions>
darwinSystemOptionsFromJSON(const llvm::json::Value &Value);
} // namespace neverd::emulation
#endif
