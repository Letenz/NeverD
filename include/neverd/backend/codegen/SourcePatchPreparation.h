//===- SourcePatchPreparation.h - Source installation preparation -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared, byte-authenticated preparation for IR and native-object patches.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_SOURCEPATCHPREPARATION_H
#define NEVERD_BACKEND_CODEGEN_SOURCEPATCHPREPARATION_H

#include "neverd/backend/codegen/SourcePatchBindings.h"
#include "neverd/backend/codegen/SourcePatchCallback.h"
#include "neverd/backend/codegen/SourcePatchDependencies.h"
#include "neverd/loader/BinaryImage.h"

#include <array>

namespace neverd {
struct SourcePatchPreparation {
  struct Callback {
    std::string Symbol;
    SourceCallbackPrefix Prefix;
    std::vector<uint8_t> Frame;
    uint64_t FrameVA = 0;
    bool WindowsUnwind = false;
  };
  BinaryImage Image;
  std::array<uint8_t, 32> OriginalSHA256{};
  std::vector<SourcePatchBinding> Bindings;
  std::vector<Callback> Callbacks;
  std::vector<SourcePatchImport> Imports;
};

/// Create callback storage in a private byte image. Publication still belongs
/// exclusively to publishSourcePatch; this operation creates no output file.
llvm::Expected<SourcePatchPreparation>
prepareSourcePatchCallbacks(const BinaryImage &Original,
                            llvm::ArrayRef<SourcePatchMapping> Mappings,
                            llvm::ArrayRef<SourcePatchBinding> Bindings);

/// Authenticate the original input and every prepared callback against the
/// final, reloaded candidate before the single publication transaction.
llvm::Error verifySourcePatchPreparation(const BinaryImage &Original,
                                         const SourcePatchPreparation &Prepared,
                                         const BinaryImage &Final);
} // namespace neverd
#endif
