//===- MachOMetadataPointer.cpp - Analysis-only pointer targets ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Resolve local metadata without turning a dyld signing recipe into a raw
/// pointer proof. Consumers still validate the destination record's layout.
//===----------------------------------------------------------------------===//

#include "neverd/loader/MachO/MachOMetadataPointer.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/MachOArm64e.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd::macho_loader {

std::optional<va_t> readMetadataPointerTarget(const BinaryImage &Image,
                                              va_t Slot) {
  const unsigned Width = Image.getPointerSize();
  if (Image.Format != BinaryFormat::MachO ||
      Image.MachOChainedFixupsAmbiguous ||
      (Width != sizeof(uint32_t) && Width != arm64e::kPointerBytes) ||
      Width > InvalidVA - Slot)
    return std::nullopt;
  const auto *Owner = Image.getSegmentFor(Slot);
  if (!Owner || !Owner->isReadable() || Owner->Size > InvalidVA - Owner->VA ||
      !rangeInBounds(Slot - Owner->VA, Width, Owner->Size) ||
      !rangeInBounds(Slot - Owner->VA, Width, Owner->FileSz))
    return std::nullopt;
  const auto *Bytes = Image.readVA(Slot, Width);
  if (!Bytes)
    return std::nullopt;
  for (const auto &Other : Image.Segments)
    if (&Other != Owner && Other.Size &&
        (Other.VA <= Slot ? Slot - Other.VA < Other.Size
                          : Other.VA - Slot < Width))
      return std::nullopt;
  const auto Auth = Image.MachOAuthenticatedPointers.find(Slot);
  const bool HasAuth = Auth != Image.MachOAuthenticatedPointers.end();
  if (!HasAuth && Image.hasMachOAuthenticatedPointerIn(Slot, Width))
    return std::nullopt;
  const va_t First = Slot >= Width - 1 ? Slot - (Width - 1) : 0;
  for (va_t At = First; At < Slot + Width; ++At) {
    if (Image.ImportPtrSlots.count(At) || Image.ImportStorageSlots.count(At) ||
        Image.DyldBindSlots.count(At) ||
        Image.ConflictingImportStorageSlots.count(At) ||
        (At != Slot && Image.MachOAuthenticatedPointers.count(At)))
      return std::nullopt;
    if (HasAuth && (Image.MachOResolvedChainedPointerSlots.count(At) ||
                    Image.CodePtrRelocSlots.count(At) ||
                    Image.DataPtrRelocSlots.count(At)))
      return std::nullopt;
  }
  const uint64_t Raw = Width == arm64e::kPointerBytes ? readLE<uint64_t>(Bytes)
                                                      : readLE<uint32_t>(Bytes);
  if (!HasAuth) {
    if (Raw && Image.MachOHasChainedFixups &&
        !Image.MachOResolvedChainedPointerSlots.count(Slot))
      return std::nullopt;
    return Raw;
  }
  const auto &Recipe = Auth->second;
  if (Image.Arch != Arch::AArch64 || Width != arm64e::kPointerBytes ||
      !Recipe.EncodedValue || *Recipe.EncodedValue != Raw ||
      !arm64e::kAuthentication.extract(Raw) ||
      Recipe.Key != arm64e::kKey.extract(Raw) ||
      Recipe.Diversity != arm64e::kDiversity.extract(Raw) ||
      Recipe.AddressDiversity != bool(arm64e::kAddressDiversity.extract(Raw)) ||
      Recipe.Target == InvalidVA)
    return std::nullopt;
  const auto *Target = Image.getSegmentFor(Recipe.Target);
  if (!Target || !Target->isReadable() ||
      Target->Size > InvalidVA - Target->VA ||
      !rangeInBounds(Recipe.Target - Target->VA, 1, Target->FileSz) ||
      !Image.readVA(Recipe.Target, 1))
    return std::nullopt;
  for (const auto &Other : Image.Segments)
    if (&Other != Target && Other.contains(Recipe.Target))
      return std::nullopt;
  return Recipe.Target;
}

} // namespace neverd::macho_loader
