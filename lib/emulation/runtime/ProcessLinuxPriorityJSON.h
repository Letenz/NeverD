//===- ProcessLinuxPriorityJSON.h - Task priority input parsing --*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSLINUXPRIORITYJSON_H
#define NEVERD_EMULATION_PROCESSLINUXPRIORITYJSON_H
#include "neverd/emulation/LinuxPriorityOptions.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
namespace neverd::emulation {
llvm::Expected<LinuxPriorityOptions>
linuxPriorityOptionsFromJSON(const llvm::json::Value &Value);
} // namespace neverd::emulation
#endif
