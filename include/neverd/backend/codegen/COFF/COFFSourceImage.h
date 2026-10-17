//===- COFFSourceImage.h - Compiler-produced PE replacement images --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_CODEGEN_COFF_COFFSOURCEIMAGE_H
#define NEVERD_BACKEND_CODEGEN_COFF_COFFSOURCEIMAGE_H

#include "neverd/backend/codegen/SourcePatchCallback.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <vector>

namespace neverd {
struct BinaryImage;
struct CompiledImage;

/// Native object evidence is separate from the rewrite backend's opaque MC
/// identities. Ranges come from bounded COFF function/section definitions;
/// absolute fields come only from validated native relocations.
struct COFFObjectFunctionRange {
  uint64_t BeginVA = 0, EndVA = 0;
};
struct COFFObjectBaseRelocation {
  uint64_t VA = 0;
  unsigned Type = 0, Width = 0;
};
struct COFFObjectEvidence {
  std::vector<COFFObjectFunctionRange> Functions;
  std::vector<COFFObjectBaseRelocation> BaseRelocations;
};

/// Install genuine ReplacementIR output. Preserve original runtime rows and
/// relocations, register new compiler records, split writable state from code,
/// and reread the result. This does not assert lifted-source EH equivalence.
llvm::Error installCOFFSourceImage(std::vector<uint8_t> &Binary,
                                   const BinaryImage &Original,
                                   const CompiledImage &Compiled,
                                   const COFFObjectEvidence *Object = nullptr);
/// Original-prefix evidence is independently rederived from the input bytes
/// and native unwind program. It is neither native-object nor MC provenance.
struct COFFOriginalCallback {
  SourceCallbackPrefix Prefix;
  uint64_t UnwindVA = 0;
};
llvm::Error
installCOFFCallbackImage(std::vector<uint8_t> &Binary,
                         const BinaryImage &Original,
                         const CompiledImage &Storage,
                         llvm::ArrayRef<COFFOriginalCallback> Callbacks);

} // namespace neverd
#endif
