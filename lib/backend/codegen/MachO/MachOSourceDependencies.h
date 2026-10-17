//===- MachOSourceDependencies.h - Source patch support -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Install explicit dynamic library imports.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_MACHO_SOURCEDEPENDENCIES_H
#define NEVERD_LIB_BACKEND_CODEGEN_MACHO_SOURCEDEPENDENCIES_H
#include "neverd/backend/codegen/SourcePatchDependencies.h"
namespace neverd {
llvm::Expected<std::vector<uint8_t>>
appendMachOSourceDependencies(const BinaryImage &Original,
                              std::vector<SourcePatchImport> &Imports);
llvm::Error
verifyMachOSourceDependencies(const BinaryImage &Image,
                              llvm::ArrayRef<SourcePatchImport> Imports);
} // namespace neverd
#endif
