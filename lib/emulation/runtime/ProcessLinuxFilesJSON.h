//===- ProcessLinuxFilesJSON.h - Explicit file wire inputs ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSLINUXFILESJSON_H
#define NEVERD_EMULATION_PROCESSLINUXFILESJSON_H

#include "neverd/emulation/LinuxFileOptions.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation {
llvm::Expected<LinuxFileOptions>
linuxFileOptionsFromJSON(const llvm::json::Value &Value);
} // namespace neverd::emulation
#endif
