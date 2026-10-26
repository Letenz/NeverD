//===- PatchIO.h - Byte patch persistence and export -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Strict patch row serialization and exclusive binary export.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PATCH_PATCHIO_H
#define NEVERD_PATCH_PATCHIO_H

#include "neverd/patch/PatchOverlay.h"

#include "llvm/Support/JSON.h"

namespace neverd::patch {

llvm::Expected<std::vector<uint8_t>> parseHex(llvm::StringRef Text,
                                              uint64_t Limit);
/// Emit canonical rows in file-offset order with lexicographically ordered
/// keys. The caller chooses compact protocol or indented sidecar formatting;
/// neither needs an intermediate JSON object tree.
void writePatchRows(llvm::json::OStream &Output, const PatchOverlay &Overlay);
llvm::Expected<std::vector<PatchRow>> parsePatchRows(llvm::StringRef Text);

/// Replays the overlay onto the original input; never replaces an existing
/// output. Explicit unsigned export removes format signature metadata.
llvm::Error exportPatches(const BinaryImage &Img, const InputFile &Input,
                          const PatchOverlay &Overlay,
                          const std::filesystem::path &Output,
                          bool AllowUnsigned = false);

} // namespace neverd::patch

#endif // NEVERD_PATCH_PATCHIO_H
