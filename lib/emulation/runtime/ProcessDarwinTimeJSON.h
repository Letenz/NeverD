//===- ProcessDarwinTimeJSON.h - Explicit Darwin time inputs ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSDARWINTIMEJSON_H
#define NEVERD_EMULATION_PROCESSDARWINTIMEJSON_H

#include "neverd/emulation/DarwinTimeOptions.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

namespace neverd::emulation {
llvm::Expected<DarwinTimeOptions>
darwinTimeOptionsFromJSON(const llvm::json::Value &Value);
} // namespace neverd::emulation
#endif
