//===- DataSymbolMetadata.h - Opaque external symbols ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the attachment that marks an undefined global standing for another
/// module's symbol whose address the module takes without knowing its type:
/// relocated data, or a function a pointer starts at before any call proves
/// its signature.  C names such a global through an opaque assembly-name alias
/// instead of an invented declaration that could conflict with a header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_DATASYMBOLMETADATA_H
#define NEVERD_BACKEND_DATASYMBOLMETADATA_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Metadata.h"

namespace neverd::data_symbol {

inline constexpr llvm::StringLiteral Attachment("neverd.data-symbol");

inline void mark(llvm::GlobalVariable &Global) {
  Global.setMetadata(Attachment, llvm::MDNode::get(Global.getContext(), {}));
}

inline bool isMarked(const llvm::GlobalVariable &Global) {
  return Global.getMetadata(Attachment) != nullptr;
}

} // namespace neverd::data_symbol

#endif // NEVERD_BACKEND_DATASYMBOLMETADATA_H
