//===- ELFSectionPatch.cpp - Patch section registration -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/ELF/ELFSectionPatch.h"

#include "neverd/object/ELFLayout.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

#include <limits>

namespace neverd {

llvm::Error registerELFPatchSections(std::vector<uint8_t> &Binary,
                                     llvm::ArrayRef<CompiledSection> Sections) {
  auto Fail = [](const llvm::Twine &Detail) {
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "ELF patch sections: " + Detail);
  };
  ELFHeaderInfo Header;
  if (!validateELFHeaderTables(Binary.data(), Binary.size(), Header))
    return Fail("invalid or unsupported header tables");
  std::vector<ELFShdrFields> Headers;
  forEachELFShdr(Binary.data(), Binary.size(),
                 [&](const ELFShdrFields &Section, uint16_t) {
                   Headers.push_back(Section);
                 });
  const auto &OldStrings = Headers[Header.ShStrNdx];
  if (OldStrings.Type != llvm::ELF::SHT_STRTAB ||
      (OldStrings.Flags & llvm::ELF::SHF_ALLOC) ||
      !rangeInBounds(OldStrings.Offset, OldStrings.Size, Binary.size()) ||
      OldStrings.Size == 0 || Binary[OldStrings.Offset] != 0 ||
      Binary[OldStrings.Offset + OldStrings.Size - 1] != 0)
    return Fail("invalid or allocated section-name table");
  std::vector<uint8_t> Names(Binary.begin() + OldStrings.Offset,
                             Binary.begin() + OldStrings.Offset +
                                 OldStrings.Size);
  for (const auto &Section : Sections) {
    if (!Section.IsAllocated || !Section.IsInImage || Section.Size == 0)
      continue;
    if (Section.Name.empty() || llvm::StringRef(Section.Name).contains('\0') ||
        !llvm::isPowerOf2_64(Section.Alignment) ||
        Section.VA % Section.Alignment ||
        Section.Size > UINT64_MAX - Section.VA)
      return Fail("invalid patch section extent or alignment");
    uint32_t Required = llvm::ELF::PF_R;
    uint64_t Flags = llvm::ELF::SHF_ALLOC;
    using Kind = llvm::mc_rewrite::RewriteSectionKind;
    if (Section.Kind == Kind::Code) {
      Required |= llvm::ELF::PF_X;
      Flags |= llvm::ELF::SHF_EXECINSTR;
    } else if (Section.Kind == Kind::WritableData ||
               Section.Kind == Kind::UninitializedData) {
      Required |= llvm::ELF::PF_W;
      Flags |= llvm::ELF::SHF_WRITE;
    }
    std::optional<uint64_t> Offset;
    bool Ambiguous = false;
    forEachELFPhdr(
        Binary.data(), Binary.size(),
        [&](const ELFPhdrFields &Segment, const uint8_t *, bool) {
          if (Segment.Type != llvm::ELF::PT_LOAD ||
              Section.VA < Segment.VAddr ||
              !rangeInBounds(Section.VA - Segment.VAddr, Section.Size,
                             Segment.FileSz) ||
              (Segment.Flags & Required) != Required ||
              !rangeInBounds(Segment.Offset, Segment.FileSz, Binary.size()))
            return;
          Ambiguous |= Offset.has_value();
          Offset = Segment.Offset + Section.VA - Segment.VAddr;
        });
    if (!Offset || Ambiguous || *Offset % Section.Alignment)
      return Fail("patch section has no unique compatible file-backed mapping");
    for (const auto &Existing : Headers) {
      if (!(Existing.Flags & llvm::ELF::SHF_ALLOC) || Existing.Size == 0)
        continue;
      if (Existing.Size > UINT64_MAX - Existing.Addr ||
          (Section.VA < Existing.Addr + Existing.Size &&
           Existing.Addr < Section.VA + Section.Size))
        return Fail("patch section overlaps an existing section");
    }
    if (Section.Name.size() > UINT32_MAX - 8 ||
        Names.size() > UINT32_MAX - Section.Name.size() - 8)
      return Fail("section-name table exceeds ELF limits");
    ELFShdrFields Added;
    Added.Name = static_cast<uint32_t>(Names.size());
    const std::string Name = ".neverd" + Section.Name;
    Names.insert(Names.end(), Name.begin(), Name.end());
    Names.push_back(0);
    Added.Type = llvm::ELF::SHT_PROGBITS;
    Added.Flags = Flags;
    Added.Addr = Section.VA;
    Added.Offset = *Offset;
    Added.Size = Section.Size;
    Added.AddrAlign = Section.Alignment;
    if (!Header.Is64 &&
        (Added.Addr > UINT32_MAX || Added.Offset > UINT32_MAX ||
         Added.Size > UINT32_MAX || Added.AddrAlign > UINT32_MAX))
      return Fail("patch section exceeds ELF32 limits");
    Headers.push_back(Added);
  }
  if (Headers.size() == Header.ShNum)
    return llvm::Error::success();
  if (Headers.size() >= llvm::ELF::SHN_LORESERVE)
    return Fail("extended section indices are not supported");
  if (Names.size() > SIZE_MAX - Binary.size() - 8)
    return Fail("section table size overflows");
  const uint64_t StringOffset = Binary.size();
  const uint64_t TableOffset = llvm::alignTo(StringOffset + Names.size(), 8);
  const uint64_t TableSize = Headers.size() * Header.ShEntSize;
  if (TableSize > SIZE_MAX - TableOffset ||
      (!Header.Is64 && TableOffset + TableSize > UINT32_MAX))
    return Fail("section table exceeds file limits");
  Headers[Header.ShStrNdx].Offset = StringOffset;
  Headers[Header.ShStrNdx].Size = Names.size();
  std::vector<uint8_t> Candidate = Binary;
  Candidate.insert(Candidate.end(), Names.begin(), Names.end());
  Candidate.resize(TableOffset + TableSize, 0);
  for (size_t I = 0; I < Headers.size(); ++I)
    writeELFShdr(Candidate.data() + TableOffset + I * Header.ShEntSize,
                 Header.Is64, Headers[I]);
  setELFShdrTable(Candidate.data(), Header.Is64, TableOffset, Headers.size());
  Binary.swap(Candidate);
  return llvm::Error::success();
}

} // namespace neverd
