//===- BytecodeSourceEvidence.h - Validate observation source bindings ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bytecode report adapter for the format-independent source evidence model.

#ifndef NEVERD_PIPELINE_BYTECODESOURCEEVIDENCE_H
#define NEVERD_PIPELINE_BYTECODESOURCEEVIDENCE_H

#include "neverd/analysis/BytecodeSource.h"
#include "neverd/backend/c/CSourceEvidence.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

namespace neverd {
llvm::Expected<std::vector<CSourceEvidence>> bindBytecodeSourceEvidence(
    const llvm::json::Value &Report, llvm::ArrayRef<uint8_t> Code,
    llvm::StringRef Profile, llvm::ArrayRef<MedFunc> Functions,
    const std::map<va_t,
                   std::vector<analysis::BytecodeStateFunction::StoreOrigin>>
        &Stores);
}
#endif
