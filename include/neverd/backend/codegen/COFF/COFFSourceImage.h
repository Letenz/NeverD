//===- COFFSourceImage.h - Compiler-produced PE replacement images --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_CODEGEN_COFF_COFFSOURCEIMAGE_H
#define NEVERD_BACKEND_CODEGEN_COFF_COFFSOURCEIMAGE_H

#include "llvm/Support/Error.h"

#include <cstdint>
#include <vector>

namespace neverd {
struct BinaryImage;
struct CompiledImage;

/// Install genuine ReplacementIR output. Preserve original runtime rows and
/// relocations, register new compiler records, split writable state from code,
/// and reread the result. This does not assert lifted-source EH equivalence.
llvm::Error installCOFFSourceImage(std::vector<uint8_t> &Binary,
                                   const BinaryImage &Original,
                                   const CompiledImage &Compiled);
} // namespace neverd
#endif
