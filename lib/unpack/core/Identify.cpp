//===- Identify.cpp - Protector evidence in an input image ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "Format.h"
#include "Packer.h"

namespace neverd::unpack {
PackerIdentification identify(const InputImage &Image) {
  PackerIdentification Out;
  for (const Packer *P : packers()) {
    std::vector<PackerEvidence> Found;
    P->collectEvidence(Image, Found);
    // The stub that runs first is the outermost one; the first protector
    // whose evidence is complete is named, and every observation is kept.
    if (Out.Kind == PackerKind::Unidentified &&
        Found.size() >= P->requiredEvidence())
      Out.Kind = P->kind();
    Out.Evidence.insert(Out.Evidence.end(), Found.begin(), Found.end());
  }
  return Out;
}

llvm::Expected<PackerIdentification>
identifyPacker(llvm::ArrayRef<uint8_t> File) {
  const Format *Selected = nullptr;
  auto Image = readImage(File, Selected);
  if (!Image)
    return Image.takeError();
  return identify(**Image);
}
} // namespace neverd::unpack
