//===- ProcessLinuxTimeJSON.h - Explicit clock wire inputs ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSLINUXTIMEJSON_H
#define NEVERD_EMULATION_PROCESSLINUXTIMEJSON_H
#include "neverd/emulation/LinuxTimeOptions.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation {
llvm::Expected<LinuxTimeOptions>
linuxTimeOptionsFromJSON(const llvm::json::Value &Value);
} // namespace neverd::emulation
#endif
