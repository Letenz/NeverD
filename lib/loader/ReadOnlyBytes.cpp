#include "neverd/loader/ReadOnlyBytes.h"

#include "neverd/loader/BinaryImage.h"

namespace neverd {
std::optional<std::vector<uint8_t>>
readImmutableImageBytes(const BinaryImage &Image, va_t Address, uint32_t Size) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.MachOChainedFixupsAmbiguous ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) || !Address ||
      Size > 1024 * 1024)
    return std::nullopt;
  // A zero-length borrow still requires a valid nonnull object address.
  const uint64_t Extent = Size ? Size : 1;
  if (Address > InvalidVA - Extent)
    return std::nullopt;
  auto Overlaps = [&](va_t Base, uint64_t Width) {
    return Width &&
           (Base <= Address ? Address - Base < Width : Base - Address < Extent);
  };
  const auto *Section = Image.getSectionFor(Address);
  const auto *Segment = Image.getSegmentFor(Address);
  if (!Section || !Segment || !Section->isReadable() || Section->isWritable() ||
      Image.isCodeAddress(Address) || !Segment->isReadable() ||
      Segment->isWritable() ||
      !rangeInBounds(Address - Section->VA, Extent, Section->Size) ||
      !rangeInBounds(Address - Section->VA, Extent, Section->FileSz) ||
      !rangeInBounds(Address - Segment->VA, Extent, Segment->Size) ||
      !rangeInBounds(Address - Segment->VA, Extent, Segment->FileSz))
    return std::nullopt;
  for (const auto &Other : Image.Sections)
    if (&Other != Section && Overlaps(Other.VA, Other.Size))
      return std::nullopt;
  for (const auto &Other : Image.Segments)
    if (&Other != Segment && Overlaps(Other.VA, Other.Size))
      return std::nullopt;
  auto SetTouches = [&](const auto &Slots) {
    auto It = Slots.lower_bound(Address >= 7 ? Address - 7 : 0);
    return It != Slots.end() && Overlaps(*It, 8);
  };
  auto MapTouches = [&](const auto &Slots) {
    auto It = Slots.lower_bound(Address >= 7 ? Address - 7 : 0);
    return It != Slots.end() && Overlaps(It->first, 8);
  };
  if (SetTouches(Image.CodePtrRelocSlots) ||
      SetTouches(Image.DataPtrRelocSlots) ||
      SetTouches(Image.RelCodeRelocSlots) ||
      SetTouches(Image.RelDataPtrRelocSlots) ||
      SetTouches(Image.MachOResolvedChainedPointerSlots) ||
      SetTouches(Image.ConflictingImportStorageSlots) ||
      MapTouches(Image.ImportPtrSlots) ||
      MapTouches(Image.ImportStorageSlots) || MapTouches(Image.DyldBindSlots) ||
      MapTouches(Image.ObjCSourceReferences))
    return std::nullopt;
  for (const auto &Relocation : Image.Relocations)
    if (Overlaps(Relocation.Address, 8))
      return std::nullopt;
  for (const auto &Relocation : Image.BaseRelocations)
    if (Overlaps(Relocation.Address, 8))
      return std::nullopt;
  for (const auto &Slot : Image.RuntimeCallablePointerSlots)
    if (Overlaps(Slot.SlotVA, 8))
      return std::nullopt;
  const auto *Bytes = Image.readVA(Address, Extent);
  return Bytes ? std::optional<std::vector<uint8_t>>(
                     std::vector<uint8_t>(Bytes, Bytes + Size))
               : std::nullopt;
}
} // namespace neverd
