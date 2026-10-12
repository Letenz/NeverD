//===- PatchObject.h - Native object replacement input ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_PATCHOBJECT_H
#define NEVERD_BACKEND_CODEGEN_PATCHOBJECT_H

#include "neverd/backend/codegen/SourcePatchBindings.h"

namespace neverd {

/// Install one native ELF x64 relocatable object, retaining its code and
/// compiler-generated unwind bytes. Only declared relocation fields change.
/// Symbol/section/relocation evidence belongs to this producer; no LLVM MC
/// owner or private function-range identity is synthesized from object names.
llvm::Expected<PatchResult> installObjectPatch(
    llvm::StringRef Object, const BinaryImage &Original,
    llvm::ArrayRef<SourcePatchMapping> Mappings,
    llvm::ArrayRef<SourcePatchBinding> Bindings,
    const std::filesystem::path &Output,
    llvm::function_ref<llvm::Error(const PatchResult &)> PrepareReceipt = {});

} // namespace neverd

#endif
