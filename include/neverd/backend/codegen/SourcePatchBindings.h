//===- SourcePatchBindings.h - Original-image references ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_SOURCEPATCHBINDINGS_H
#define NEVERD_BACKEND_CODEGEN_SOURCEPATCHBINDINGS_H

#include "neverd/backend/codegen/SourcePatch.h"

namespace neverd {

enum class SourcePatchBindingKind {
  Function,
  ReadOnlyData,
  WritableData,
  OriginalFunction
};

struct SourcePatchBinding {
  std::string Symbol;
  SourcePatchBindingKind Kind = SourcePatchBindingKind::Function;
  va_t OriginalVA = 0;
  /// Object inputs must provide a data extent. IR inputs may use zero to
  /// select their declaration's allocation size.
  uint64_t Size = 0;
};

/// Validate original-image storage and code facts shared by both producers.
/// The producer separately authenticates the external declaration/symbol.
llvm::Error
validateSourcePatchBinding(const SourcePatchBinding &Binding,
                           const BinaryImage &Image,
                           llvm::ArrayRef<SourcePatchMapping> Replacements);

/// Authenticate explicit original references and replace external declarations
/// with the existing synthetic address symbols. References retain the original
/// storage and are generated relative to the image, including under ASLR.
/// No original-body callback is synthesized for an entry being replaced.
llvm::Expected<std::vector<SourcePatchBinding>>
bindSourcePatchReferences(llvm::Module &Module, const BinaryImage &Image,
                          llvm::ArrayRef<SourcePatchMapping> Replacements,
                          llvm::ArrayRef<SourcePatchBinding> Bindings,
                          bool RenameDeclarations = true);

} // namespace neverd

#endif
