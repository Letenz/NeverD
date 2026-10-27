//===- ProcessDarwinJSON.h - Explicit Darwin image inputs -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Shared JSON admission for the Darwin startup module catalogue.
#ifndef NEVERD_EMULATION_PROCESSDARWINJSON_H
#define NEVERD_EMULATION_PROCESSDARWINJSON_H
#include "neverd/emulation/DarwinProcessOptions.h"

#include "llvm/Support/JSON.h"
namespace neverd::emulation {
llvm::Expected<DarwinProcessOptions>
darwinOptionsFromJSON(const llvm::json::Value &Value);
}
#endif
