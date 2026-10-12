//===- SourcePatch.h - Explicit source replacement requests -------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_SOURCEPATCH_H
#define NEVERD_BACKEND_CODEGEN_SOURCEPATCH_H

#include "neverd/backend/codegen/CodeGen.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/Error.h"

#include <filesystem>
#include <string>
#include <vector>

namespace neverd {

/// Shared admission for compiler-produced or externally supplied source IR.
/// A codegen triple alone does not establish a loader/unwind installation ABI.
llvm::Error validateSourcePatchTarget(Arch Architecture, BinaryFormat Format);

struct BinaryImage;

struct SourcePatchMapping {
  /// An empty name selects the sole externally visible definition.
  std::string Symbol;
  va_t OriginalVA = 0;
};

/// Validate all requested entries before attaching their source identities.
/// With an empty request, resolve the legacy sub_HEX/export-name convention
/// into explicit mappings. Helpers never receive an inferred identity when
/// the caller supplies mappings.
llvm::Expected<std::vector<SourcePatchMapping>>
bindSourcePatch(llvm::Module &Module, const BinaryImage &Image,
                llvm::ArrayRef<SourcePatchMapping> Requested);

/// Install a bound module into an independent output file. Every requested
/// source must have an exact installed receipt before publication. Full/lift
/// rewriting deliberately keeps its separate preserve-original/no-op policy.
/// PrepareReceipt runs after verification but before publication so caller
/// reporting allocations cannot fail after committing the output.
llvm::Expected<PatchResult> installSourcePatch(
    llvm::Module &Module, const BinaryImage &Image,
    llvm::ArrayRef<SourcePatchMapping> Mappings,
    const std::filesystem::path &OutputPath, llvm::StringRef TextSection = {},
    llvm::function_ref<llvm::Error(const PatchResult &)> PrepareReceipt = {});

/// Shared transaction for IR and object installers. Each installer supplies
/// its own source evidence; publication checks exact receipts and entry bytes.
llvm::Expected<PatchResult> publishSourcePatch(
    const BinaryImage &Image, llvm::ArrayRef<SourcePatchMapping> Mappings,
    const std::filesystem::path &OutputPath,
    llvm::function_ref<llvm::Expected<PatchResult>(
        const std::filesystem::path &, const std::filesystem::path &)>
        Install,
    llvm::function_ref<llvm::Error(const PatchResult &)> PrepareReceipt = {});

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_SOURCEPATCH_H
