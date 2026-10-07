//===- Identify.cpp - Protector evidence in an input image ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "Format.h"

namespace neverd::unpack {
llvm::Expected<PackerIdentification>
identifyPacker(llvm::ArrayRef<uint8_t> File) {
  const Format *Selected = nullptr;
  auto Image = readImage(File, Selected);
  if (!Image)
    return Image.takeError();
  // Preserve the identification API and report shape for existing clients.
  // Recovery uses execution evidence and has no protector-specific registry.
  return PackerIdentification{};
}
} // namespace neverd::unpack
