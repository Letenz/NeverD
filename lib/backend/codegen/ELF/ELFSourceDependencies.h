//===- ELFSourceDependencies.h - Source patch support -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve ELF loader tables while adding imports.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_ELF_SOURCEDEPENDENCIES_H
#define NEVERD_LIB_BACKEND_CODEGEN_ELF_SOURCEDEPENDENCIES_H
#include "neverd/backend/codegen/SourcePatchDependencies.h"
namespace neverd {
llvm::Expected<std::vector<uint8_t>>
appendELFSourceDependencies(const BinaryImage &Original,
                            std::vector<SourcePatchImport> &Imports);
llvm::Error
verifyELFSourceDependencies(const BinaryImage &Image,
                            llvm::ArrayRef<SourcePatchImport> Imports);
} // namespace neverd
#endif
