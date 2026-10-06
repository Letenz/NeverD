//===- Image.cpp - Container-neutral input image facts --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "Format.h"
#include "Packer.h"

namespace neverd::unpack {
InputImage::~InputImage() = default;
Format::~Format() = default;
Packer::~Packer() = default;

const ImageRegion *InputImage::regionAt(uint64_t RVA) const {
  for (const auto &R : Regions)
    if (RVA >= R.RVA && RVA - R.RVA < R.MemorySize)
      return &R;
  return nullptr;
}

llvm::ArrayRef<uint8_t> InputImage::fileBytesFrom(uint64_t RVA) const {
  const auto *R = regionAt(RVA);
  if (!R || RVA - R->RVA >= R->FileSize)
    return {};
  return File.slice(R->FileOffset, R->FileSize).drop_front(RVA - R->RVA);
}

std::optional<uint64_t> Packer::declaredEntry(const InputImage &) const {
  return std::nullopt;
}
void Packer::planRebuild(const InputImage &, const Capture &,
                         RebuildPlan &) const {}
} // namespace neverd::unpack
