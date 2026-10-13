//===- LLVMCStorage.h - C object storage contracts -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared alignment spelling for LLVM local and global object declarations.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_LLVMC_STORAGE_H
#define NEVERD_BACKEND_C_LLVMC_STORAGE_H

#include "llvm/IR/DataLayout.h"
#include "llvm/Support/Alignment.h"

#include <string>

namespace neverd::llvmc {

inline std::string storageAlignment(llvm::Type *Type,
                                    const llvm::DataLayout &Layout,
                                    llvm::Align Alignment) {
  // Preserve explicit over-alignment without weakening the C object's natural
  // alignment. Locals, globals and their forward declarations share this rule.
  if (Alignment <= Layout.getABITypeAlign(Type))
    return {};
  return "_Alignas(" + std::to_string(Alignment.value()) + ") ";
}

} // namespace neverd::llvmc

#endif
