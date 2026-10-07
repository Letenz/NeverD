//===- ProcessLinuxKernelJSON.h - Kernel input parsing ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSLINUXKERNELJSON_H
#define NEVERD_EMULATION_PROCESSLINUXKERNELJSON_H
#include "neverd/emulation/LinuxKernelOptions.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
namespace neverd::emulation {
llvm::Expected<LinuxKernelOptions>
linuxKernelOptionsFromJSON(const llvm::json::Value &Value);
} // namespace neverd::emulation
#endif
