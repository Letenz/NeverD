//===- PatchStorage.cpp - Replacement storage layout ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/PatchStorage.h"

#include "neverd/ArchSupport.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

namespace neverd {

uint64_t sourcePatchStorageAlignment(Arch Architecture, BinaryFormat Format) {
  if (Format == BinaryFormat::ELF)
    return elfPageSize(Architecture);
  if (Format == BinaryFormat::MachO)
    return machoPageSize(Architecture);
  return 64 * 1024;
}

llvm::Expected<PatchStorageLayout>
validatePatchStorageLayout(const CompiledImage &Image, uint64_t PageAlignment) {
  auto Fail = [](const llvm::Twine &Detail) {
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "patch storage: " + Detail);
  };
  if (!Image.Success || !llvm::isPowerOf2_64(PageAlignment) ||
      Image.Bytes.size() > InvalidVA - Image.BaseVA)
    return Fail("invalid image or page alignment");
  std::optional<uint64_t> Writable;
  uint64_t ReadOnlyEnd = 0;
  for (const auto &Section : Image.Sections) {
    if (!Section.IsAllocated || Section.Size == 0)
      continue;
    if (llvm::StringRef(Section.Name).starts_with(".data.rel.ro"))
      return Fail(
          "post-relocation read-only storage requires RELRO registration");
    const bool Write =
        Section.Kind == llvm::mc_rewrite::RewriteSectionKind::WritableData ||
        Section.Kind == llvm::mc_rewrite::RewriteSectionKind::UninitializedData;
    if (!Section.IsInImage) {
      if (Write)
        return Fail("external writable storage has no installation owner");
      continue;
    }
    if (!rangeInBounds(Section.Offset, Section.Size, Image.Bytes.size()) ||
        Section.VA != Image.BaseVA + Section.Offset)
      return Fail("section exceeds its placement image");
    if (Write)
      Writable = std::min(Writable.value_or(Section.Offset), Section.Offset);
    else
      ReadOnlyEnd = std::max(ReadOnlyEnd, Section.Offset + Section.Size);
  }
  if (!Writable)
    return PatchStorageLayout{0, ReadOnlyEnd};
  if (*Writable == 0 || *Writable < ReadOnlyEnd ||
      (Image.BaseVA + *Writable) % PageAlignment != 0)
    return Fail("writable storage is not separated from code by whole pages");
  return PatchStorageLayout{*Writable, ReadOnlyEnd};
}

} // namespace neverd
