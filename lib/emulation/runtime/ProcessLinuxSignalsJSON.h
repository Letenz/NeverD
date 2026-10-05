//===- ProcessLinuxSignalsJSON.h - Signal input parsing ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSLINUXSIGNALSJSON_H
#define NEVERD_EMULATION_PROCESSLINUXSIGNALSJSON_H

#include "neverd/emulation/LinuxSignalOptions.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

namespace neverd::emulation {
llvm::Expected<LinuxSignalOptions>
linuxSignalOptionsFromJSON(const llvm::json::Value &Value);
} // namespace neverd::emulation
#endif
