#include "neverd/loader/ReadOnlyBytes.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Support/Endian.h"

#include <set>

namespace neverd {
namespace {
bool supportedImage(const BinaryImage &Image) {
  return Image.Format == BinaryFormat::MachO && Image.Bits == Bitness::Bits64 &&
         supportsImmutableImageReads(Image);
}

bool supportedPEImage(const BinaryImage &Image) {
  return Image.Format == BinaryFormat::COFF &&
         supportsImmutableImageReads(Image);
}

bool supportedImmutableCodeImage(const BinaryImage &Image) {
  return supportedImage(Image) || supportedPEImage(Image) ||
         (Image.Format == BinaryFormat::ELF && !Image.IsRelocatable &&
          Image.Arch == Arch::ARM && Image.Bits == Bitness::Bits32);
}

bool overlaps(va_t Address, uint64_t Extent, va_t Base, uint64_t Width) {
  return Width && Extent &&
         (Base <= Address ? Address - Base < Width : Base - Address < Extent);
}

const uint8_t *mappedBytes(const BinaryImage &Image, va_t Address,
                           uint64_t Extent, bool Immutable, bool Code = false) {
  if (!Address || Address > InvalidVA - Extent)
    return nullptr;
  if (Code) {
    if (!Image.isCodeRange(Address, Extent))
      return nullptr;
  } else {
    const auto End = Image.mappedObjectOwnerEnd(Address);
    if (!End || *End < Address || Extent > *End - Address)
      return nullptr;
  }
  const auto *Section = Image.getSectionFor(Address);
  const auto *Segment = Image.getSegmentFor(Address);
  if (!Section && Code && Image.Format == BinaryFormat::ELF &&
      Image.Arch == Arch::ARM && Image.ARMReachabilityConstrained && Segment &&
      Segment->isReadable() && Segment->isExecutable() &&
      Image.instructionModeAt(Address) &&
      (!Immutable || Segment->ReadOnlyAfterRelocations ||
       !Segment->isWritable()) &&
      rangeInBounds(Address - Segment->VA, Extent, Segment->Size) &&
      rangeInBounds(Address - Segment->VA, Extent, Segment->FileSz)) {
    for (const auto &Other : Image.Segments)
      if (&Other != Segment && overlaps(Address, Extent, Other.VA, Other.Size))
        return nullptr;
    return Image.readVA(Address, Extent);
  }
  if (!Section || !Segment || !Section->isReadable() ||
      !Segment->isReadable() || Image.isCodeAddress(Address) != Code ||
      (Code && (!Section->isExecutable() || !Segment->isExecutable())) ||
      (Immutable && !Segment->ReadOnlyAfterRelocations &&
       (Section->isWritable() || Segment->isWritable())) ||
      Section->Size > InvalidVA - Section->VA ||
      Segment->Size > InvalidVA - Segment->VA || Section->VA < Segment->VA ||
      !rangeInBounds(Address - Section->VA, Extent, Section->Size) ||
      !rangeInBounds(Address - Section->VA, Extent, Section->FileSz) ||
      !rangeInBounds(Address - Segment->VA, Extent, Segment->Size) ||
      !rangeInBounds(Address - Segment->VA, Extent, Segment->FileSz) ||
      Section->FileOff < Segment->FileOff ||
      Section->FileOff - Segment->FileOff != Section->VA - Segment->VA)
    return nullptr;
  if (Image.Format == BinaryFormat::MachO) {
    const auto Type = Section->Type & llvm::MachO::SECTION_TYPE;
    if (Type == llvm::MachO::S_ZEROFILL || Type == llvm::MachO::S_GB_ZEROFILL ||
        Type == llvm::MachO::S_THREAD_LOCAL_ZEROFILL)
      return nullptr;
  }
  for (const auto &Other : Image.Sections)
    if (&Other != Section && overlaps(Address, Extent, Other.VA, Other.Size))
      return nullptr;
  for (const auto &Other : Image.Segments)
    if (&Other != Segment && overlaps(Address, Extent, Other.VA, Other.Size))
      return nullptr;
  return Image.readVA(Address, Extent);
}

// Byte copies admit no fixup. Each pointer reader admits only its exact
// normalized slot kind, with independently checked resolution and target.
bool hasConflictingFixups(const BinaryImage &Image, va_t Address,
                          uint64_t Extent, bool Pointer, bool Import = false,
                          bool ObjCReference = false, bool CodePointer = false,
                          bool ExactPEPointer = false) {
  const uint64_t PointerWidth = Image.getPointerSize();
  auto Touches = [&](const auto &Slots, auto Key, bool AllowExact = false) {
    auto It = Slots.lower_bound(
        Address >= PointerWidth - 1 ? Address - (PointerWidth - 1) : 0);
    for (; It != Slots.end() &&
           (Key(*It) < Address || Key(*It) - Address < Extent);
         ++It)
      if (overlaps(Address, Extent, Key(*It), PointerWidth) &&
          !(AllowExact && Key(*It) == Address))
        return true;
    return false;
  };
  auto Set = [](va_t Slot) { return Slot; };
  auto Map = [](const auto &Slot) { return Slot.first; };
  if (Touches(Image.CodePtrRelocSlots, Set, CodePointer) ||
      Touches(Image.DataPtrRelocSlots, Set, Pointer) ||
      Touches(Image.DataPtrRelocTargetOwners, Map, Pointer) ||
      Touches(Image.RelCodeRelocSlots, Set) ||
      Touches(Image.RelDataPtrRelocSlots, Set) ||
      Touches(Image.MachOResolvedChainedPointerSlots, Set,
              Pointer || Import || CodePointer) ||
      Touches(Image.ConflictingImportStorageSlots, Set) ||
      Touches(Image.ImportPtrSlots, Map, Import) ||
      Touches(Image.ImportStorageSlots, Map, Import) ||
      Touches(Image.DyldBindSlots, Map, Import) ||
      Touches(Image.ObjCSourceReferences, Map, ObjCReference) ||
      Touches(Image.DataAddressRelocOperands, Map) ||
      Touches(Image.CodeAddressRelocOperands, Map))
    return true;
  for (const auto &Relocation : Image.Relocations)
    if (overlaps(Address, Extent, Relocation.Address, 8))
      return true;
  for (const auto &Relocation : Image.BaseRelocations) {
    const uint64_t Width =
        Relocation.Type == llvm::COFF::IMAGE_REL_BASED_HIGHLOW ? 4 : 8;
    if (overlaps(Address, Extent, Relocation.Address, Width) &&
        !(ExactPEPointer && Relocation.Address == Address &&
          Width == PointerWidth &&
          (Relocation.Type == llvm::COFF::IMAGE_REL_BASED_HIGHLOW ||
           Relocation.Type == llvm::COFF::IMAGE_REL_BASED_DIR64)))
      return true;
  }
  for (const auto &Slot : Image.RuntimeCallablePointerSlots)
    if (overlaps(Address, Extent, Slot.SlotVA, PointerWidth))
      return true;
  return false;
}
} // namespace

bool supportsImmutableImageReads(const BinaryImage &Image) {
  if (Image.IsRelocatable || (Image.Format != BinaryFormat::COFF &&
                              Image.Format != BinaryFormat::ELF &&
                              Image.Format != BinaryFormat::MachO))
    return false;
  if (Image.Format == BinaryFormat::MachO &&
      (Image.MachOChainedFixupsAmbiguous ||
       (Image.MachOHasChainedFixups && Image.Bits != Bitness::Bits64)))
    return false;
  return (Image.Bits == Bitness::Bits32 &&
          (Image.Arch == Arch::X86 || Image.Arch == Arch::ARM)) ||
         (Image.Bits == Bitness::Bits64 &&
          (Image.Arch == Arch::X64 || Image.Arch == Arch::AArch64));
}

std::optional<std::vector<uint8_t>>
readImmutableImageBytes(const BinaryImage &Image, va_t Address, uint32_t Size) {
  if (!supportsImmutableImageReads(Image) || Size > 1024 * 1024)
    return std::nullopt;
  // A zero-length borrow still requires a valid nonnull object address.
  const uint64_t Extent = Size ? Size : 1;
  const auto *Bytes = mappedBytes(Image, Address, Extent, true);
  if (!Bytes || hasConflictingFixups(Image, Address, Extent, false))
    return std::nullopt;
  return std::vector<uint8_t>(Bytes, Bytes + Size);
}

std::optional<std::vector<uint8_t>>
readInitialImageBytes(const BinaryImage &Image, va_t Address, uint32_t Size) {
  if (!supportedImage(Image) || !Size || Size > 1024 * 1024)
    return std::nullopt;
  const auto *Bytes = mappedBytes(Image, Address, Size, false);
  if (!Bytes || hasConflictingFixups(Image, Address, Size, false))
    return std::nullopt;
  return std::vector<uint8_t>(Bytes, Bytes + Size);
}

std::optional<std::vector<uint8_t>>
readImmutableCodeBytes(const BinaryImage &Image, va_t Address, uint32_t Size) {
  if (!supportedImmutableCodeImage(Image) || !Size || Size > 1024 * 1024)
    return std::nullopt;
  const auto *Bytes = mappedBytes(Image, Address, Size, true, true);
  if (!Bytes || hasConflictingFixups(Image, Address, Size, false))
    return std::nullopt;
  return std::vector<uint8_t>(Bytes, Bytes + Size);
}

std::optional<std::vector<uint8_t>>
readImmutablePE32CodeBytes(const BinaryImage &Image, va_t Address,
                           uint32_t Size,
                           const std::vector<va_t> &AbsoluteOperands) {
  if (Image.Format != BinaryFormat::COFF || Image.IsRelocatable ||
      Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 || !Size ||
      Size > 1024 * 1024 || Address > UINT32_MAX ||
      Size > uint64_t(UINT32_MAX) + 1 - Address)
    return std::nullopt;
  const auto *Bytes = mappedBytes(Image, Address, Size, true, true);
  if (!Bytes)
    return std::nullopt;
  std::set<va_t> Operands;
  for (va_t Operand : AbsoluteOperands)
    if (Operand < Address || Operand - Address > Size ||
        Size - (Operand - Address) < 4 || !Operands.insert(Operand).second)
      return std::nullopt;
  for (auto It = Operands.begin(); It != Operands.end(); ++It)
    if (It != Operands.begin() && *It - *std::prev(It) < 4)
      return std::nullopt;
  std::set<va_t> Relocated;
  for (const auto &Relocation : Image.BaseRelocations)
    if (Relocation.Type &&
        overlaps(Address, Size, Relocation.Address,
                 Relocation.Type == llvm::COFF::IMAGE_REL_BASED_HIGHLOW ? 4
                                                                        : 8)) {
      if (Relocation.Type != llvm::COFF::IMAGE_REL_BASED_HIGHLOW ||
          !Operands.count(Relocation.Address) ||
          !Relocated.insert(Relocation.Address).second)
        return std::nullopt;
    }
  for (const auto &Relocation : Image.Relocations)
    if (overlaps(Address, Size, Relocation.Address, 8))
      return std::nullopt;
  auto CheckedFields = [&](const auto &Fields) {
    for (const auto &[Slot, Field] : Fields)
      if (overlaps(Address, Size, Slot, Field.Width)) {
        if (!Operands.count(Slot) || !Relocated.count(Slot) ||
            Field.Width != 4 || Field.PCRelativeFromInstructionEnd ||
            Field.Kind != RelocatedAddressFieldKind::Generic ||
            Field.TargetVA !=
                llvm::support::endian::read32le(Bytes + (Slot - Address)))
          return false;
      }
    return true;
  };
  if (!CheckedFields(Image.CodeAddressRelocOperands) ||
      !CheckedFields(Image.DataAddressRelocOperands))
    return std::nullopt;
  for (const auto *Slots :
       {&Image.CodePtrRelocSlots, &Image.DataPtrRelocSlots,
        &Image.RelCodeRelocSlots, &Image.RelDataPtrRelocSlots})
    for (va_t Slot : *Slots)
      if (overlaps(Address, Size, Slot, 4))
        return std::nullopt;
  for (const auto &Slot : Image.RuntimeCallablePointerSlots)
    if (overlaps(Address, Size, Slot.SlotVA, 4))
      return std::nullopt;
  return std::vector<uint8_t>(Bytes, Bytes + Size);
}

std::optional<uint64_t> readImmutableChainedImageValue(const BinaryImage &Image,
                                                       va_t Address) {
  if (!supportedImage(Image) || !Image.MachOHasChainedFixups ||
      !Image.MachOResolvedChainedPointerSlots.count(Address))
    return std::nullopt;
  const auto *Bytes = mappedBytes(Image, Address, 8, true);
  if (!Bytes || hasConflictingFixups(Image, Address, 8, true))
    return std::nullopt;
  return llvm::support::endian::read64le(Bytes);
}

bool isImagePointerBitPattern(const BinaryImage &Image, uint64_t Bits,
                              uint16_t Width) {
  return Bits && Width == (Image.is64Bit() ? 8 : 4) &&
         Image.getSectionFor(Bits);
}

bool isFileBackedWritableImageRange(const BinaryImage &Image, va_t Address,
                                    uint32_t Size) {
  if (!supportedImage(Image) || !Size || Size > 1024 * 1024 ||
      !mappedBytes(Image, Address, Size, false))
    return false;
  const auto *Section = Image.getSectionFor(Address);
  const auto *Segment = Image.getSegmentFor(Address);
  return Section->isWritable() && !Section->isExecutable() &&
         Segment->isWritable() && !Segment->isExecutable() &&
         !Segment->ReadOnlyAfterRelocations;
}

namespace {
bool immutableLocalCodeTarget(const BinaryImage &Image, va_t Target) {
  return Image.Arch != Arch::ARM &&
         (Image.Arch != Arch::AArch64 || Target % 4 == 0) &&
         Image.hasAuthenticatedFunctionEntryAt(Target) &&
         !Image.findImportStubAt(Target) &&
         readImmutableCodeBytes(Image, Target,
                                Image.Arch == Arch::AArch64 ? 4 : 1)
             .has_value();
}

std::optional<va_t> readResolvedPointer(const BinaryImage &Image, va_t Address,
                                        bool Immutable,
                                        bool SelectorReference = false) {
  if (!supportedImage(Image) || !Image.DataPtrRelocSlots.count(Address) ||
      (Image.MachOHasChainedFixups &&
       !Image.MachOResolvedChainedPointerSlots.count(Address)))
    return std::nullopt;
  const auto Owner = Image.DataPtrRelocTargetOwners.find(Address);
  const auto *Bytes = mappedBytes(Image, Address, 8, Immutable);
  if (!Bytes || Owner == Image.DataPtrRelocTargetOwners.end() ||
      hasConflictingFixups(Image, Address, 8, true, false, SelectorReference))
    return std::nullopt;
  const auto Target = llvm::support::endian::read64le(Bytes);
  if (!mappedBytes(Image, Target, 1, false) ||
      Image.getSectionFor(Target)->VA != Owner->second)
    return std::nullopt;
  return Target;
}
std::optional<va_t> readImmutablePEPointer(const BinaryImage &Image,
                                           va_t Address, bool Code) {
  if (supportedPEImage(Image)) {
    const unsigned Width = Image.getPointerSize();
    const auto &Slots =
        Code ? Image.CodePtrRelocSlots : Image.DataPtrRelocSlots;
    if ((Width != 4 && Width != 8) || !Slots.count(Address))
      return std::nullopt;
    const unsigned Type = Width == 8 ? llvm::COFF::IMAGE_REL_BASED_DIR64
                                     : llvm::COFF::IMAGE_REL_BASED_HIGHLOW;
    unsigned ExactRelocations = 0;
    for (const auto &Relocation : Image.BaseRelocations)
      if (Relocation.Address == Address) {
        if (Relocation.Type != Type || ++ExactRelocations != 1)
          return std::nullopt;
      }
    if (ExactRelocations != 1)
      return std::nullopt;
    const auto *Bytes = mappedBytes(Image, Address, Width, true);
    const auto Owner = Image.DataPtrRelocTargetOwners.find(Address);
    if (!Bytes || (!Code && Owner == Image.DataPtrRelocTargetOwners.end()) ||
        hasConflictingFixups(Image, Address, Width, !Code, false, false, Code,
                             true))
      return std::nullopt;
    const uint64_t Target = Width == 8 ? llvm::support::endian::read64le(Bytes)
                                       : llvm::support::endian::read32le(Bytes);
    if (!mappedBytes(Image, Target, 1, false, Code) ||
        (!Code && Image.getSectionFor(Target)->VA != Owner->second) ||
        (Code && !immutableLocalCodeTarget(Image, Target)))
      return std::nullopt;
    return Target;
  }
  return std::nullopt;
}
} // namespace

std::optional<va_t> readImmutableImagePointer(const BinaryImage &Image,
                                              va_t Address) {
  if (supportedPEImage(Image))
    return readImmutablePEPointer(Image, Address, false);
  return readResolvedPointer(Image, Address, true);
}

std::optional<va_t> readImmutableImageCodePointer(const BinaryImage &Image,
                                                  va_t Address) {
  if (supportedPEImage(Image))
    return readImmutablePEPointer(Image, Address, true);
  if (!supportedImage(Image) || Address % 8 || !Image.MachOHasChainedFixups ||
      !Image.CodePtrRelocSlots.count(Address) ||
      !Image.MachOResolvedChainedPointerSlots.count(Address))
    return std::nullopt;
  const auto *Bytes = mappedBytes(Image, Address, 8, true);
  if (!Bytes ||
      hasConflictingFixups(Image, Address, 8, false, false, false, true))
    return std::nullopt;
  const auto Target = llvm::support::endian::read64le(Bytes);
  if (!immutableLocalCodeTarget(Image, Target))
    return std::nullopt;
  return Target;
}

namespace {
bool imageImportSlot(const BinaryImage &Image, va_t Address, bool Immutable,
                     bool ClassReference) {
  if (!supportedImage(Image) || !mappedBytes(Image, Address, 8, Immutable) ||
      hasConflictingFixups(Image, Address, 8, false, true, ClassReference))
    return false;
  const auto Bind = Image.DyldBindSlots.find(Address);
  if (Bind == Image.DyldBindSlots.end() || Bind->second.Name.empty() ||
      Bind->second.Module.empty() || Bind->second.WeakImport ||
      Bind->second.Addend ||
      !Image.isValidImportStorageSlot(Address, Bind->second.Name))
    return false;
  const auto Storage = Image.collectImportStorageSlot(Address);
  const auto Slot = Storage.Slots.find(Address);
  return !Storage.Conflicts.count(Address) && Slot != Storage.Slots.end() &&
         Slot->second.Name == Bind->second.Name && !Slot->second.Addend;
}
} // namespace

bool isImmutableImageImportSlot(const BinaryImage &Image, va_t Address) {
  return imageImportSlot(Image, Address, true, false);
}

bool isInitialImageImportSlot(const BinaryImage &Image, va_t Address) {
  return imageImportSlot(Image, Address, false, false);
}

std::optional<uint64_t> readImmutableImageIvarOffset(const BinaryImage &Image,
                                                     va_t Address) {
  const auto Ref = Image.ObjCSourceReferences.find(Address);
  const auto *Section = Image.getSectionFor(Address);
  if (!supportedImage(Image) || Address % 8 || !Section ||
      (Section->Type & llvm::MachO::SECTION_TYPE) != llvm::MachO::S_REGULAR ||
      Ref == Image.ObjCSourceReferences.end() ||
      Ref->second.TheKind != ObjCSourceReference::Kind::IvarOffset ||
      Ref->second.Address != Address || Ref->second.Size != 8 ||
      Ref->second.Name.empty() || Ref->second.ClassName.empty())
    return std::nullopt;
  const auto *Bytes = mappedBytes(Image, Address, 8, true);
  if (!Bytes || hasConflictingFixups(Image, Address, 8, false, false, true))
    return std::nullopt;
  return llvm::support::endian::read64le(Bytes);
}

bool isImmutableImageClassImportSlot(const BinaryImage &Image, va_t Address) {
  const auto Ref = Image.ObjCSourceReferences.find(Address);
  const auto Bind = Image.DyldBindSlots.find(Address);
  if (Address % 8 || Ref == Image.ObjCSourceReferences.end() ||
      Bind == Image.DyldBindSlots.end() || Ref->second.Address != Address ||
      Ref->second.Size != 8 || Ref->second.Name.empty() ||
      Ref->second.TheKind != ObjCSourceReference::Kind::Class ||
      Bind->second.Name != "_OBJC_CLASS_$_" + Ref->second.Name)
    return false;
  return imageImportSlot(Image, Address, true, true);
}

std::optional<va_t> readInitialImagePointer(const BinaryImage &Image,
                                            va_t Address) {
  return readResolvedPointer(Image, Address, false);
}
std::optional<va_t> readInitialImageSelectorPointer(const BinaryImage &Image,
                                                    va_t Address) {
  const auto R = Image.ObjCSourceReferences.find(Address);
  if (Image.Format != BinaryFormat::MachO || Address % 8 ||
      R == Image.ObjCSourceReferences.end() || R->second.Address != Address ||
      R->second.Size != 8 || R->second.Name.empty() ||
      R->second.TheKind != ObjCSourceReference::Kind::Selector)
    return std::nullopt;
  return readResolvedPointer(Image, Address, false, true);
}
} // namespace neverd
