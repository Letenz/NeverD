#ifndef NEVERD_LOADER_OBJC_OBJCSOURCEDECLARATIONS_H
#define NEVERD_LOADER_OBJC_OBJCSOURCEDECLARATIONS_H

#include "neverd/ir/SourceTypeHint.h"

#include "llvm/ADT/StringRef.h"

#include <optional>

namespace neverd {
struct BinaryImage;

/// Merge every known declaration for a dynamically dispatched selector.
/// Runtime declarations and applicable framework declarations must agree;
/// unsupported or variadic declarations veto the source call signature.
/// This supplies source-call ABI facts, never a dynamic receiver identity or
/// permission to rewrite a method implementation.
std::optional<SourceFunctionTypeHint>
objcSelectorSourceTypeHint(const BinaryImage &Image, llvm::StringRef Selector);
} // namespace neverd
#endif
