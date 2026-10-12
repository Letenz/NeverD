//===- ELFSectionPatch.h - Patch section registration -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_ELF_ELFSECTIONPATCH_H
#define NEVERD_BACKEND_CODEGEN_ELF_ELFSECTIONPATCH_H

#include "neverd/backend/codegen/BinaryRewriter.h"

namespace neverd {

/// Register allocated patch sections after their bytes have been installed in
/// load segments. Existing section indices and names retain their meanings;
/// patch sections receive a `.neverd` prefix. Header/string tables are appended
/// outside load mappings. Failure leaves Binary unchanged.
llvm::Error registerELFPatchSections(std::vector<uint8_t> &Binary,
                                     llvm::ArrayRef<CompiledSection> Sections);

} // namespace neverd

#endif
