//===- ELFSourceSymbols.h - Source replacement references ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_ELF_ELFSOURCESYMBOLS_H
#define NEVERD_BACKEND_CODEGEN_ELF_ELFSOURCESYMBOLS_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/IR/Module.h"
#include "llvm/MC/BinaryRewrite.h"

namespace neverd {

/// Preserve original CPU/security requirements; current entry replacement
/// refuses CET IBT images until it can retain their ENDBR landing instruction.
llvm::Error validateELFSourceFeatures(const BinaryImage &Original);
llvm::Expected<uint32_t>
readELFX64PatchPropertyNote(llvm::ArrayRef<uint8_t> Bytes);

/// Resolve against authenticated original code or loader-owned pointer slots.
/// GOT references never resolve to the pointed-to code/data address. Direct
/// data references require an explicit, validated source binding.
llvm::Expected<std::optional<uint64_t>> resolveELFSourceSymbol(
    const BinaryImage &Image, const llvm::Module &Module,
    const llvm::mc_rewrite::RewriteSymbolResolveRequest &Request);

} // namespace neverd

#endif
