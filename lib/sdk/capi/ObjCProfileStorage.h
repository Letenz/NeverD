#ifndef NEVERD_SDK_CAPI_OBJCPROFILESTORAGE_H
#define NEVERD_SDK_CAPI_OBJCPROFILESTORAGE_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/MachO.h"

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd::sdk {

/// The counter section contains numeric storage, unlike profile descriptors
/// which contain pointers. Rebuild a whole section to preserve overlapping
/// accesses and interior aliases across independently emitted methods. This
/// snapshot is shared by all projections in one export, never cached by VA
/// across images. It does not reconnect the original profiling runtime.
class ObjCProfileStorage {
  std::map<va_t, std::vector<uint8_t>> Sections;

public:
  explicit ObjCProfileStorage(const BinaryImage &Image) {
    if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
        Image.Bits != Bitness::Bits64 ||
        (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
        Image.MachOChainedFixupsAmbiguous)
      return;
    // Conservatively reserve eight bytes at every pointer/fixup occurrence,
    // including a slot beginning just before the section. Do not use target
    // address inventories here: those describe references *to* this storage.
    std::set<va_t> PointerSlots;
    auto AddSet = [&](const auto &Values) {
      PointerSlots.insert(Values.begin(), Values.end());
    };
    auto AddMap = [&](const auto &Values) {
      for (const auto &[Address, Value] : Values)
        PointerSlots.insert(Address);
    };
    AddSet(Image.CodePtrRelocSlots);
    AddSet(Image.DataPtrRelocSlots);
    AddSet(Image.RelDataPtrRelocSlots);
    AddSet(Image.RelCodeRelocSlots);
    AddSet(Image.MachOResolvedChainedPointerSlots);
    AddSet(Image.ConflictingImportStorageSlots);
    AddMap(Image.ImportPtrSlots);
    AddMap(Image.ImportStorageSlots);
    AddMap(Image.DyldBindSlots);
    AddMap(Image.ObjCSourceReferences);
    for (const auto &Relocation : Image.Relocations)
      PointerSlots.insert(Relocation.Address);
    for (const auto &Relocation : Image.BaseRelocations)
      PointerSlots.insert(Relocation.Address);
    for (const auto &Slot : Image.RuntimeCallablePointerSlots)
      PointerSlots.insert(Slot.SlotVA);
    auto Overlaps = [](va_t A, uint64_t AS, va_t B, uint64_t BS) {
      return AS && BS && (A <= B ? B - A < AS : A - B < BS);
    };
    for (const auto &Section : Image.Sections) {
      if (Section.Name != "__llvm_prf_cnts" ||
          Section.SegmentName != "__DATA" || !Section.VA || !Section.Size ||
          Section.Size > 16 * 1024 * 1024 ||
          Section.Size > UINT64_MAX - Section.VA ||
          Section.FileSz != Section.Size || !Section.isReadable() ||
          !Section.isWritable() || Section.isExecutable() ||
          (Section.Type & llvm::MachO::SECTION_TYPE) != llvm::MachO::S_REGULAR)
        continue;
      const auto *Segment = Image.getSegmentFor(Section.VA);
      if (!Segment || Segment->Name != Section.SegmentName ||
          !Segment->isReadable() || !Segment->isWritable() ||
          Segment->isExecutable() ||
          Section.VA - Segment->VA > Segment->FileSz ||
          Section.Size > Segment->FileSz - (Section.VA - Segment->VA) ||
          Section.Size > Segment->Size - (Section.VA - Segment->VA))
        continue;
      bool Ambiguous = false;
      for (const auto &Other : Image.Sections)
        if (&Other != &Section &&
            Overlaps(Section.VA, Section.Size, Other.VA, Other.Size))
          Ambiguous = true;
      for (const auto &Other : Image.Segments)
        if (&Other != Segment &&
            Overlaps(Section.VA, Section.Size, Other.VA, Other.Size))
          Ambiguous = true;
      const auto Slot =
          PointerSlots.lower_bound(Section.VA >= 7 ? Section.VA - 7 : 0);
      if (Ambiguous || (Slot != PointerSlots.end() &&
                        Overlaps(Section.VA, Section.Size, *Slot, 8)))
        continue;
      const auto *Bytes = Image.readVA(Section.VA, Section.Size);
      if (Bytes)
        Sections.emplace(Section.VA,
                         std::vector<uint8_t>(Bytes, Bytes + Section.Size));
    }
  }

  std::optional<va_t> sectionFor(va_t Address, uint64_t Width) const {
    if (!Width)
      return std::nullopt;
    auto It = Sections.upper_bound(Address);
    if (It == Sections.begin())
      return std::nullopt;
    --It;
    const auto Offset = Address - It->first;
    if (Offset >= It->second.size() || Width > It->second.size() - Offset)
      return std::nullopt;
    return It->first;
  }

  bool contains(va_t Base) const { return Sections.count(Base); }

  static std::string helperName(va_t Base) {
    return "neverd_profile_counters_" + llvm::utohexstr(Base, true) +
           "_address";
  }

  std::string render(const std::set<va_t> &Used,
                     std::set<std::string> &SharedFunctions) const {
    std::string Source;
    for (va_t Base : Used) {
      const auto &Bytes = Sections.at(Base);
      const auto Name = helperName(Base);
      SharedFunctions.insert(Name);
      Source += "\nuintptr_t " + Name +
                "(void) {\n"
                "  static unsigned char counters[" +
                std::to_string(Bytes.size()) + "] = { ";
      // Sparse initializers keep the common all-zero section compact while
      // retaining every nonzero byte in a captured image.
      bool Any = false;
      for (size_t I = 0; I < Bytes.size(); ++I)
        if (Bytes[I]) {
          if (Any)
            Source += ", ";
          Source += "[" + std::to_string(I) + "] = " + std::to_string(Bytes[I]);
          Any = true;
        }
      if (!Any)
        Source += "0";
      Source += " };\n  return (uintptr_t)counters;\n}\n";
    }
    return Source;
  }
};

} // namespace neverd::sdk
#endif
