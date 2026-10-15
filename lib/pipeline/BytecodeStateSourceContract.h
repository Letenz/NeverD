//===- BytecodeStateSourceContract.h - Parse source ABI contracts -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// JSON transport for the shared, format-independent state source model.

#ifndef NEVERD_PIPELINE_BYTECODESTATESOURCECONTRACT_H
#define NEVERD_PIPELINE_BYTECODESTATESOURCECONTRACT_H
#include "neverd/pipeline/StateSourceRecovery.h"

#include "llvm/Support/JSON.h"
namespace neverd {
llvm::Expected<StateSourceContract>
readBytecodeStateSourceContract(const llvm::json::Value &JSON,
                                unsigned StateBytes);
}
#endif
