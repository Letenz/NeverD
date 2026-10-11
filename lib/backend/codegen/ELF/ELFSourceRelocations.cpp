//===- ELFSourceRelocations.cpp - Load-time replacement pointers ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/ELF/ELFSourceRelocations.h"

#include "neverd/backend/codegen/PatchStorage.h"
#include "neverd/object/ELFLayout.h"

#include "llvm/MC/MCFixup.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

#include <map>
#include <set>

namespace neverd {
namespace {
using ELF = llvm::object::ELF64LE;

llvm::Error fail(const llvm::Twine &Detail) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "ELF source relocations: " + Detail);
}

struct DynamicLayout {
  ELFHeaderInfo Header;
  // Tag -> (value, file offset of Elf64_Dyn).
  std::map<int64_t, std::pair<uint64_t, uint64_t>> Tags;
  std::vector<ELFShdrFields> Sections;
};

llvm::Expected<uint64_t> fileOffset(llvm::ArrayRef<uint8_t> Binary, uint64_t VA,
                                    uint64_t Size) {
  std::optional<uint64_t> Offset;
  unsigned Count = 0;
  forEachELFPhdr(
      Binary.data(), Binary.size(),
      [&](const ELFPhdrFields &Segment, const uint8_t *, bool) {
        if (Segment.Type != llvm::ELF::PT_LOAD || VA < Segment.VAddr ||
            !rangeInBounds(VA - Segment.VAddr, Size, Segment.FileSz) ||
            !rangeInBounds(Segment.Offset, Segment.FileSz, Binary.size()))
          return;
        Offset = Segment.Offset + VA - Segment.VAddr;
        ++Count;
      });
  if (Count != 1)
    return fail("metadata has no unique file-backed mapping");
  return *Offset;
}

llvm::Expected<DynamicLayout> dynamicLayout(llvm::ArrayRef<uint8_t> Binary) {
  DynamicLayout Result;
  if (!validateELFHeaderTables(Binary.data(), Binary.size(), Result.Header) ||
      !Result.Header.Is64)
    return fail("ELF64 header required");
  ELF::Ehdr Header;
  std::memcpy(&Header, Binary.data(), sizeof(Header));
  if (Header.e_machine != llvm::ELF::EM_X86_64 ||
      (Header.e_type != llvm::ELF::ET_EXEC &&
       Header.e_type != llvm::ELF::ET_DYN))
    return fail("load-time pointer registration currently requires ELF x64");
  std::optional<ELFPhdrFields> Dynamic;
  unsigned Count = 0;
  forEachELFPhdr(Binary.data(), Binary.size(),
                 [&](const ELFPhdrFields &Segment, const uint8_t *, bool) {
                   if (Segment.Type == llvm::ELF::PT_DYNAMIC) {
                     Dynamic = Segment;
                     ++Count;
                   }
                 });
  if (Count != 1 || !Dynamic || Dynamic->FileSz % sizeof(ELF::Dyn) ||
      !rangeInBounds(Dynamic->Offset, Dynamic->FileSz, Binary.size()))
    return fail("a unique bounded PT_DYNAMIC is required");
  auto Mapped = fileOffset(Binary, Dynamic->VAddr, Dynamic->FileSz);
  if (!Mapped)
    return Mapped.takeError();
  if (*Mapped != Dynamic->Offset)
    return fail("PT_DYNAMIC disagrees with its load segment");
  bool Terminated = false;
  for (uint64_t I = 0; I < Dynamic->FileSz; I += sizeof(ELF::Dyn)) {
    ELF::Dyn Item;
    const uint64_t Offset = Dynamic->Offset + I;
    std::memcpy(&Item, Binary.data() + Offset, sizeof(Item));
    if (Item.getTag() == llvm::ELF::DT_NULL) {
      Terminated = true;
      break;
    }
    const int64_t Tag = Item.getTag();
    if (Tag == llvm::ELF::DT_RELA || Tag == llvm::ELF::DT_RELASZ ||
        Tag == llvm::ELF::DT_RELAENT || Tag == llvm::ELF::DT_SYMTAB ||
        Tag == llvm::ELF::DT_SYMENT || Tag == llvm::ELF::DT_STRTAB ||
        Tag == llvm::ELF::DT_STRSZ) {
      if (!Result.Tags.emplace(Tag, std::make_pair(Item.getVal(), Offset))
               .second)
        return fail("duplicate dynamic table tag");
    }
  }
  if (!Terminated)
    return fail("unterminated dynamic table");
  forEachELFShdr(Binary.data(), Binary.size(),
                 [&](const ELFShdrFields &Section, uint16_t) {
                   Result.Sections.push_back(Section);
                 });
  return Result;
}

} // namespace

llvm::Expected<std::optional<uint32_t>>
findELFSourceDynamicSymbol(const BinaryImage &Original, llvm::StringRef Name) {
  auto Layout = dynamicLayout(Original.Raw);
  if (!Layout)
    return Layout.takeError();
  const auto &Tags = Layout->Tags;
  for (int64_t Tag : {llvm::ELF::DT_SYMTAB, llvm::ELF::DT_SYMENT,
                      llvm::ELF::DT_STRTAB, llvm::ELF::DT_STRSZ})
    if (!Tags.count(Tag))
      return fail("missing dynamic symbol/string table");
  if (Tags.at(llvm::ELF::DT_SYMENT).first != sizeof(ELF::Sym))
    return fail("invalid dynamic symbol entry size");
  const uint64_t VA = Tags.at(llvm::ELF::DT_SYMTAB).first;
  const ELFShdrFields *Symbols = nullptr;
  for (const auto &Section : Layout->Sections)
    if (Section.Type == llvm::ELF::SHT_DYNSYM && Section.Addr == VA) {
      if (Symbols)
        return fail("ambiguous dynamic symbol table");
      Symbols = &Section;
    }
  if (!Symbols || Symbols->EntSize != sizeof(ELF::Sym) ||
      Symbols->Size % sizeof(ELF::Sym) ||
      Symbols->Link >= Layout->Sections.size())
    return fail("invalid dynamic symbol section");
  auto SymOffset = fileOffset(Original.Raw, VA, Symbols->Size);
  if (!SymOffset)
    return SymOffset.takeError();
  const auto &Strings = Layout->Sections[Symbols->Link];
  if (*SymOffset != Symbols->Offset || Strings.Type != llvm::ELF::SHT_STRTAB ||
      Strings.Addr != Tags.at(llvm::ELF::DT_STRTAB).first ||
      Strings.Size != Tags.at(llvm::ELF::DT_STRSZ).first)
    return fail("dynamic symbols disagree with section identity");
  auto StrOffset = fileOffset(Original.Raw, Strings.Addr, Strings.Size);
  if (!StrOffset)
    return StrOffset.takeError();
  if (*StrOffset != Strings.Offset)
    return fail("dynamic string table disagrees with section identity");
  llvm::StringRef Names(
      reinterpret_cast<const char *>(Original.Raw.data() + *StrOffset),
      Strings.Size);
  std::optional<uint32_t> Match;
  for (uint64_t I = 1; I < Symbols->Size / sizeof(ELF::Sym); ++I) {
    ELF::Sym Symbol;
    std::memcpy(&Symbol, Original.Raw.data() + *SymOffset + I * sizeof(Symbol),
                sizeof(Symbol));
    if (Symbol.st_name >= Names.size())
      return fail("invalid dynamic symbol name");
    auto Tail = Names.drop_front(Symbol.st_name);
    const size_t End = Tail.find('\0');
    if (End == llvm::StringRef::npos)
      return fail("unterminated dynamic symbol name");
    if (Tail.take_front(End) != Name)
      continue;
    if (Match || I > UINT32_MAX ||
        Symbol.getBinding() != llvm::ELF::STB_GLOBAL ||
        Symbol.getType() == llvm::ELF::STT_TLS ||
        Symbol.getType() == llvm::ELF::STT_GNU_IFUNC)
      return fail("ambiguous or unsupported dynamic symbol: " + Name);
    Match = static_cast<uint32_t>(I);
  }
  return Match;
}

llvm::Expected<ELFSourceRelocationTable>
prepareELFSourceRelocations(const BinaryImage &Original,
                            const llvm::Module &Module, CompiledImage &Compiled,
                            uint64_t MetadataOffset) {
  std::vector<ELFSourcePointerRelocation> Pointers;
  for (const auto &Section : Compiled.Sections) {
    if (!Section.IsAllocated || !Section.IsInImage)
      continue;
    for (const auto &Fixup : Section.FixupReferences) {
      if (Fixup.IsPCRel || !Fixup.SubtractSymbol.empty())
        continue;
      // ISA instruction fixups are not absolute pointer storage. Plain data
      // fixups retain that distinction in the compiler's authenticated kind.
      if (Fixup.Kind != llvm::FK_Data_8 && Fixup.Kind != llvm::FK_Data_4)
        continue;
      if (Fixup.Specifier || Fixup.Kind != llvm::FK_Data_8 ||
          Fixup.BitWidth != 64 || Original.Arch != Arch::X64)
        return fail("unsupported absolute symbolic field: " + Section.Name);
      if (Section.Kind != llvm::mc_rewrite::RewriteSectionKind::WritableData ||
          !rangeInBounds(Fixup.Offset, 8, Section.Size) || !Fixup.IsResolved ||
          Fixup.Symbol.empty())
        return fail("absolute pointer lacks resolved writable storage: " +
                    Section.Name);
      const uint64_t Field = Section.VA + Fixup.Offset;
      ELFSourcePointerRelocation Relocation;
      Relocation.FieldVA = Field;
      const auto *Declaration = Module.getNamedValue(Fixup.Symbol);
      const bool Local = Compiled.SymbolAddrs.count(Fixup.Symbol) ||
                         (Declaration && Declaration->isDSOLocal());
      if (!Local) {
        auto Symbol = findELFSourceDynamicSymbol(Original, Fixup.Symbol);
        if (!Symbol)
          return Symbol.takeError();
        if (!*Symbol)
          return fail("absolute external has no existing dynamic symbol: " +
                      Fixup.Symbol);
        Relocation.DynamicSymbol = **Symbol;
        Relocation.Addend = Fixup.Addend;
      } else {
        Relocation.Addend = static_cast<int64_t>(Fixup.ResolvedValue);
      }
      Pointers.push_back(Relocation);
    }
  }
  return prepareELFPointerRelocations(Original, Compiled, Pointers,
                                      MetadataOffset);
}

llvm::Expected<ELFSourceRelocationTable> prepareELFPointerRelocations(
    const BinaryImage &Original, CompiledImage &Compiled,
    llvm::ArrayRef<ELFSourcePointerRelocation> Pointers,
    uint64_t MetadataOffset) {
  std::vector<ELF::Rela> Added;
  std::set<uint64_t> Fields;
  for (const auto &Pointer : Pointers) {
    bool Owned = false;
    for (const auto &Section : Compiled.Sections)
      if (Section.IsAllocated && Section.IsInImage &&
          (Section.Kind == llvm::mc_rewrite::RewriteSectionKind::WritableData ||
           Section.Kind ==
               llvm::mc_rewrite::RewriteSectionKind::UninitializedData) &&
          Pointer.FieldVA >= Section.VA &&
          rangeInBounds(Pointer.FieldVA - Section.VA, 8, Section.Size))
        Owned = true;
    if (!Owned)
      return fail("absolute pointer lacks bounded writable storage");
    auto Next = Fields.lower_bound(Pointer.FieldVA);
    if ((Next != Fields.end() && *Next - Pointer.FieldVA < 8) ||
        (Next != Fields.begin() && Pointer.FieldVA - *std::prev(Next) < 8))
      return fail("overlapping pointer relocation fields");
    Fields.insert(Pointer.FieldVA);
    ELF::Rela Relocation{};
    Relocation.r_offset = Pointer.FieldVA;
    Relocation.setSymbolAndType(Pointer.DynamicSymbol,
                                Pointer.DynamicSymbol
                                    ? llvm::ELF::R_X86_64_64
                                    : llvm::ELF::R_X86_64_RELATIVE,
                                false);
    Relocation.r_addend = Pointer.Addend;
    Added.push_back(Relocation);
  }
  if (Added.empty())
    return ELFSourceRelocationTable{};
  auto Layout = dynamicLayout(Original.Raw);
  if (!Layout)
    return Layout.takeError();
  for (int64_t Tag :
       {llvm::ELF::DT_RELA, llvm::ELF::DT_RELASZ, llvm::ELF::DT_RELAENT})
    if (!Layout->Tags.count(Tag))
      return fail("existing DT_RELA table required");
  const auto &Address = Layout->Tags.at(llvm::ELF::DT_RELA);
  const auto &Size = Layout->Tags.at(llvm::ELF::DT_RELASZ);
  if (Layout->Tags.at(llvm::ELF::DT_RELAENT).first != sizeof(ELF::Rela) ||
      Size.first % sizeof(ELF::Rela))
    return fail("invalid original RELA table size");
  auto Offset = fileOffset(Original.Raw, Address.first, Size.first);
  if (!Offset)
    return Offset.takeError();
  std::optional<uint64_t> SectionHeader;
  for (size_t I = 0; I < Layout->Sections.size(); ++I) {
    const auto &Section = Layout->Sections[I];
    if (Section.Type == llvm::ELF::SHT_RELA && Section.Addr == Address.first) {
      if (SectionHeader || Section.Offset != *Offset ||
          Section.Size != Size.first || Section.EntSize != sizeof(ELF::Rela))
        return fail("RELA table disagrees with section identity");
      SectionHeader = Layout->Header.ShOff + I * Layout->Header.ShEntSize;
    }
  }
  if (!SectionHeader)
    return fail("RELA section identity missing");
  auto Storage = validatePatchStorageLayout(Compiled, 4096);
  if (!Storage)
    return Storage.takeError();
  if (MetadataOffset > UINT64_MAX - 7 ||
      Added.size() > (UINT64_MAX - Size.first) / sizeof(ELF::Rela))
    return fail("relocation table size or alignment overflows");
  MetadataOffset = llvm::alignTo(MetadataOffset, 8);
  const uint64_t TableSize = Size.first + Added.size() * sizeof(ELF::Rela);
  if (!Storage->WritableOffset || MetadataOffset < Storage->ReadOnlySize ||
      !rangeInBounds(MetadataOffset, TableSize, Storage->WritableOffset))
    return fail("relocation table exceeds reserved read-only padding");
  std::vector<uint8_t> Table(Original.Raw.begin() + *Offset,
                             Original.Raw.begin() + *Offset + Size.first);
  for (const auto &Relocation : Added) {
    const auto *Bytes = reinterpret_cast<const uint8_t *>(&Relocation);
    Table.insert(Table.end(), Bytes, Bytes + sizeof(Relocation));
  }
  std::copy(Table.begin(), Table.end(),
            Compiled.Bytes.begin() + MetadataOffset);
  return ELFSourceRelocationTable{Compiled.BaseVA + MetadataOffset, TableSize,
                                  Address.second, Size.second, *SectionHeader};
}

llvm::Error installELFSourceRelocations(std::vector<uint8_t> &Binary,
                                        const ELFSourceRelocationTable &Table) {
  if (!Table.Size)
    return llvm::Error::success();
  auto Offset = fileOffset(Binary, Table.VA, Table.Size);
  if (!Offset)
    return Offset.takeError();
  for (uint64_t Field : {Table.AddressTagOffset, Table.SizeTagOffset})
    if (!rangeInBounds(Field, sizeof(ELF::Dyn), Binary.size()))
      return fail("dynamic tag exceeds candidate file");
  if (!rangeInBounds(Table.SectionHeaderOffset, sizeof(ELF::Shdr),
                     Binary.size()))
    return fail("relocation section header exceeds candidate file");
  ELF::Dyn Address, Size;
  std::memcpy(&Address, Binary.data() + Table.AddressTagOffset,
              sizeof(Address));
  std::memcpy(&Size, Binary.data() + Table.SizeTagOffset, sizeof(Size));
  if (Address.getTag() != llvm::ELF::DT_RELA ||
      Size.getTag() != llvm::ELF::DT_RELASZ)
    return fail("dynamic tag identity changed during installation");
  auto Section = readELFShdr(Binary.data() + Table.SectionHeaderOffset, true);
  if (Section.Type != llvm::ELF::SHT_RELA || Section.Addr != Address.getVal() ||
      Section.Size != Size.getVal())
    return fail("relocation section identity changed during installation");
  Address.d_un.d_ptr = Table.VA;
  Size.d_un.d_val = Table.Size;
  Section.Addr = Table.VA;
  Section.Offset = *Offset;
  Section.Size = Table.Size;
  std::memcpy(Binary.data() + Table.AddressTagOffset, &Address,
              sizeof(Address));
  std::memcpy(Binary.data() + Table.SizeTagOffset, &Size, sizeof(Size));
  writeELFShdr(Binary.data() + Table.SectionHeaderOffset, true, Section);
  return llvm::Error::success();
}

} // namespace neverd
