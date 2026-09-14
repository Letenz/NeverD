#ifndef NEVERD_LOADER_MACHO_DARWINRUNTIMEIMPORT_H
#define NEVERD_LOADER_MACHO_DARWINRUNTIMEIMPORT_H

#include "neverd/loader/BinaryImage.h"

namespace neverd {
/// A source call must name one exact, non-weak runtime import. Preserve the
/// original linker spelling for the runtime-specific ABI catalog to match.
inline std::optional<llvm::StringRef>
darwinRuntimeImport(const BinaryImage &Image, va_t Slot) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      Image.ConflictingImportStorageSlots.count(Slot))
    return std::nullopt;
  const auto Import = Image.ImportPtrSlots.find(Slot);
  if (Import == Image.ImportPtrSlots.end())
    return std::nullopt;
  if (auto I = Image.ImportStorageSlots.find(Slot);
      I != Image.ImportStorageSlots.end() &&
      (I->second.Name != Import->second || I->second.Addend))
    return std::nullopt;
  if (auto I = Image.DyldBindSlots.find(Slot);
      I != Image.DyldBindSlots.end() &&
      (I->second.Name != Import->second || I->second.Addend ||
       I->second.WeakImport))
    return std::nullopt;
  return Import->second;
}
} // namespace neverd
#endif
