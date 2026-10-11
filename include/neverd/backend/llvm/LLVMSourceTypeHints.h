//===- LLVMSourceTypeHints.h - Explicit source scalar spelling -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_LLVM_LLVMSOURCETYPEHINTS_H
#define NEVERD_BACKEND_LLVM_LLVMSOURCETYPEHINTS_H

#include "llvm/ADT/StringRef.h"

namespace neverd {
// LLVM integers are signless. These producer-supplied spelling facts preserve
// an imported C declaration without inventing signext/zeroext machine ABI
// attributes. Consumers must still validate the actual LLVM scalar width.
inline constexpr llvm::StringLiteral LLVMSourceSignedResult =
    "neverd.source.signed_result";
inline constexpr llvm::StringLiteral LLVMSourceSignedParameter =
    "neverd.source.signed_parameter";
} // namespace neverd
#endif
