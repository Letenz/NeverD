//===- COFFObjectView.h - Linked PE reader projection ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LOADER_COFF_COFFOBJECTVIEW_H
#define NEVERD_LOADER_COFF_COFFOBJECTVIEW_H

#include "llvm/Object/COFF.h"
#include "llvm/Support/MemoryBuffer.h"

#include <memory>

namespace neverd::coff_loader {

/// LLVM's COFF reader requires nonzero VirtualSize for RVA lookup, including
/// directories parsed during construction. Project the linked PE fallback
/// into private reader storage; never change the caller's original snapshot.
/// Ordinary PE images and relocatable COFF objects retain the original view.
class COFFObjectView {
  std::unique_ptr<llvm::WritableMemoryBuffer> Storage;
  std::unique_ptr<llvm::object::COFFObjectFile> Object;

public:
  static llvm::Expected<COFFObjectView> create(llvm::MemoryBufferRef Input);
  const llvm::object::COFFObjectFile &object() const { return *Object; }
};

} // namespace neverd::coff_loader
#endif
