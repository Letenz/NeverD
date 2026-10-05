//===- ProcessDarwinFilesJSON.h - Darwin file wire inputs -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSDARWINFILESJSON_H
#define NEVERD_EMULATION_PROCESSDARWINFILESJSON_H
#include "neverd/emulation/DarwinFileOptions.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation {
llvm::Expected<DarwinFileOptions>
darwinFileOptionsFromJSON(const llvm::json::Value &Value);
}
#endif
