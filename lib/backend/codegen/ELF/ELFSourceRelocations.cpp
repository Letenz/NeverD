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

template <typename ELF>
llvm::Expected<DynamicLayout> dynamicLayout(llvm::ArrayRef<uint8_t> Binary) {
  DynamicLayout Result;
  if (!validateELFHeaderTables(Binary.data(), Binary.size(), Result.Header) ||
      Result.Header.Is64 != (sizeof(typename ELF::Addr) == 8))
    return fail("matching little-endian ELF header required");
  typename ELF::Ehdr Header;
  std::memcpy(&Header, Binary.data(), sizeof(Header));
  if ((Header.e_machine != llvm::ELF::EM_X86_64 &&
       Header.e_machine != llvm::ELF::EM_AARCH64 &&
       Header.e_machine != llvm::ELF::EM_386 &&
       Header.e_machine != llvm::ELF::EM_ARM) ||
      (Header.e_type != llvm::ELF::ET_EXEC &&
       Header.e_type != llvm::ELF::ET_DYN))
    return fail("unsupported ELF load-time pointer target");
  std::optional<ELFPhdrFields> Dynamic;
  unsigned Count = 0;
  forEachELFPhdr(Binary.data(), Binary.size(),
                 [&](const ELFPhdrFields &Segment, const uint8_t *, bool) {
                   if (Segment.Type == llvm::ELF::PT_DYNAMIC) {
                     Dynamic = Segment;
                     ++Count;
                   }
                 });
  if (Count != 1 || !Dynamic || Dynamic->FileSz % sizeof(typename ELF::Dyn) ||
      !rangeInBounds(Dynamic->Offset, Dynamic->FileSz, Binary.size()))
    return fail("a unique bounded PT_DYNAMIC is required");
  auto Mapped = fileOffset(Binary, Dynamic->VAddr, Dynamic->FileSz);
  if (!Mapped)
    return Mapped.takeError();
  if (*Mapped != Dynamic->Offset)
    return fail("PT_DYNAMIC disagrees with its load segment");
  bool Terminated = false;
  for (uint64_t I = 0; I < Dynamic->FileSz; I += sizeof(typename ELF::Dyn)) {
    typename ELF::Dyn Item;
    const uint64_t Offset = Dynamic->Offset + I;
    std::memcpy(&Item, Binary.data() + Offset, sizeof(Item));
    if (Item.getTag() == llvm::ELF::DT_NULL) {
      Terminated = true;
      break;
    }
    const int64_t Tag = Item.getTag();
    if (Tag == llvm::ELF::DT_RELA || Tag == llvm::ELF::DT_RELASZ ||
        Tag == llvm::ELF::DT_RELAENT || Tag == llvm::ELF::DT_REL ||
        Tag == llvm::ELF::DT_RELSZ || Tag == llvm::ELF::DT_RELENT ||
        Tag == llvm::ELF::DT_SYMTAB || Tag == llvm::ELF::DT_SYMENT ||
        Tag == llvm::ELF::DT_STRTAB || Tag == llvm::ELF::DT_STRSZ) {
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

template <typename ELF>
llvm::Expected<std::optional<uint32_t>>
findDynamicSymbol(const BinaryImage &Original, llvm::StringRef Name) {
  auto Layout = dynamicLayout<ELF>(Original.Raw);
  if (!Layout)
    return Layout.takeError();
  const auto &Tags = Layout->Tags;
  for (int64_t Tag : {llvm::ELF::DT_SYMTAB, llvm::ELF::DT_SYMENT,
                      llvm::ELF::DT_STRTAB, llvm::ELF::DT_STRSZ})
    if (!Tags.count(Tag))
      return fail("missing dynamic symbol/string table");
  if (Tags.at(llvm::ELF::DT_SYMENT).first != sizeof(typename ELF::Sym))
    return fail("invalid dynamic symbol entry size");
  const uint64_t VA = Tags.at(llvm::ELF::DT_SYMTAB).first;
  const ELFShdrFields *Symbols = nullptr;
  for (const auto &Section : Layout->Sections)
    if (Section.Type == llvm::ELF::SHT_DYNSYM && Section.Addr == VA) {
      if (Symbols)
        return fail("ambiguous dynamic symbol table");
      Symbols = &Section;
    }
  if (!Symbols || Symbols->EntSize != sizeof(typename ELF::Sym) ||
      Symbols->Size % sizeof(typename ELF::Sym) ||
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
  for (uint64_t I = 1; I < Symbols->Size / sizeof(typename ELF::Sym); ++I) {
    typename ELF::Sym Symbol;
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

llvm::Expected<std::optional<uint32_t>>
findELFSourceDynamicSymbol(const BinaryImage &Original, llvm::StringRef Name) {
  return Original.getPointerSize() == 8
             ? findDynamicSymbol<llvm::object::ELF64LE>(Original, Name)
             : findDynamicSymbol<llvm::object::ELF32LE>(Original, Name);
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
      // ARM's S_PREL31 is an expression specifier, not an MCFixup PC-relative
      // flag. Its index/descriptor fields have already been authenticated and
      // registered by the EHABI installer; they are not load-time pointers.
      if (Original.Arch == Arch::ARM && Fixup.SpecifierName == "none" &&
          llvm::StringRef(Section.Name).starts_with(".ARM.exidx") &&
          llvm::StringRef(Fixup.Symbol).starts_with("__aeabi_unwind_cpp_pr"))
        continue; // R_ARM_NONE is an unwind-runtime dependency, not a writer.
      if (Original.Arch == Arch::ARM && Fixup.SpecifierName == "prel31" &&
          (llvm::StringRef(Section.Name).starts_with(".ARM.exidx") ||
           llvm::StringRef(Section.Name).starts_with(".ARM.extab")))
        continue;
      if (Original.Arch == Arch::X86 &&
          ((Fixup.Symbol == "_GLOBAL_OFFSET_TABLE_" && !Fixup.Specifier) ||
           Fixup.SpecifierName == "GOTOFF" || Fixup.SpecifierName == "GOT"))
        continue; // The address model encodes these relative to the GOT anchor.
      const unsigned Width = Original.getPointerSize();
      const bool AArch64Absolute = Original.Arch == Arch::AArch64 &&
                                   Fixup.Specifier == 1 &&
                                   Fixup.SpecifierName == "ABS";
      if ((Fixup.Specifier && !AArch64Absolute) ||
          Fixup.Kind != (Width == 8 ? llvm::FK_Data_8 : llvm::FK_Data_4) ||
          Fixup.BitWidth != Width * 8)
        return fail("unsupported absolute symbolic field: " + Section.Name +
                    " kind=" + Fixup.KindName +
                    " specifier=" + llvm::Twine(Fixup.Specifier) +
                    " width=" + llvm::Twine(Fixup.BitWidth));
      if (Section.Kind != llvm::mc_rewrite::RewriteSectionKind::WritableData ||
          !rangeInBounds(Fixup.Offset, Width, Section.Size) ||
          !Fixup.IsResolved || Fixup.Symbol.empty())
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

template <typename ELF>
llvm::Expected<ELFSourceRelocationTable>
preparePointerRelocations(const BinaryImage &Original, CompiledImage &Compiled,
                          llvm::ArrayRef<ELFSourcePointerRelocation> Pointers,
                          uint64_t MetadataOffset) {
  constexpr bool Is64 = sizeof(typename ELF::Addr) == 8;
  constexpr unsigned Width = sizeof(typename ELF::Addr);
  using Rel = std::conditional_t<Is64, typename ELF::Rela, typename ELF::Rel>;
  constexpr int64_t AddressTag = Is64 ? llvm::ELF::DT_RELA : llvm::ELF::DT_REL;
  constexpr int64_t SizeTag = Is64 ? llvm::ELF::DT_RELASZ : llvm::ELF::DT_RELSZ;
  constexpr int64_t EntryTag =
      Is64 ? llvm::ELF::DT_RELAENT : llvm::ELF::DT_RELENT;
  uint32_t Absolute = 0, Relative = 0;
  switch (Original.Arch) {
  case Arch::X86:
    Absolute = llvm::ELF::R_386_32;
    Relative = llvm::ELF::R_386_RELATIVE;
    break;
  case Arch::X64:
    Absolute = llvm::ELF::R_X86_64_64;
    Relative = llvm::ELF::R_X86_64_RELATIVE;
    break;
  case Arch::ARM:
    Absolute = llvm::ELF::R_ARM_ABS32;
    Relative = llvm::ELF::R_ARM_RELATIVE;
    break;
  case Arch::AArch64:
    Absolute = llvm::ELF::R_AARCH64_ABS64;
    Relative = llvm::ELF::R_AARCH64_RELATIVE;
    break;
  default:
    return fail("unsupported native pointer architecture");
  }
  std::vector<Rel> Added;
  std::set<uint64_t> Fields;
  for (const auto &Pointer : Pointers) {
    bool Owned = false;
    for (const auto &Section : Compiled.Sections)
      if (Section.IsAllocated && Section.IsInImage &&
          (Section.Kind == llvm::mc_rewrite::RewriteSectionKind::WritableData ||
           Section.Kind ==
               llvm::mc_rewrite::RewriteSectionKind::UninitializedData) &&
          Pointer.FieldVA >= Section.VA &&
          rangeInBounds(Pointer.FieldVA - Section.VA, Width, Section.Size))
        Owned = true;
    if (!Owned)
      return fail("absolute pointer lacks bounded writable storage");
    auto Next = Fields.lower_bound(Pointer.FieldVA);
    if ((Next != Fields.end() && *Next - Pointer.FieldVA < Width) ||
        (Next != Fields.begin() && Pointer.FieldVA - *std::prev(Next) < Width))
      return fail("overlapping pointer relocation fields");
    Fields.insert(Pointer.FieldVA);
    Rel Relocation{};
    Relocation.r_offset = Pointer.FieldVA;
    Relocation.setSymbolAndType(Pointer.DynamicSymbol,
                                Pointer.DynamicSymbol ? Absolute : Relative,
                                false);
    if constexpr (Is64)
      Relocation.r_addend = Pointer.Addend;
    else if (Pointer.FieldVA > UINT32_MAX || Pointer.DynamicSymbol > 0xffffff ||
             (Pointer.DynamicSymbol
                  ? !llvm::isInt<32>(Pointer.Addend)
                  : Pointer.Addend < 0 ||
                        uint64_t(Pointer.Addend) > UINT32_MAX))
      return fail("ELF32 pointer relocation exceeds its field width");
    Added.push_back(Relocation);
  }
  if (Added.empty())
    return ELFSourceRelocationTable{};
  auto Layout = dynamicLayout<ELF>(Original.Raw);
  if (!Layout)
    return Layout.takeError();
  for (int64_t Tag : {AddressTag, SizeTag, EntryTag})
    if (!Layout->Tags.count(Tag))
      return fail("existing DT_RELA table required");
  const auto &Address = Layout->Tags.at(AddressTag);
  const auto &Size = Layout->Tags.at(SizeTag);
  if (Layout->Tags.at(EntryTag).first != sizeof(Rel) ||
      Size.first % sizeof(Rel))
    return fail("invalid original RELA table size");
  auto Offset = fileOffset(Original.Raw, Address.first, Size.first);
  if (!Offset)
    return Offset.takeError();
  std::optional<uint64_t> SectionHeader;
  for (size_t I = 0; I < Layout->Sections.size(); ++I) {
    const auto &Section = Layout->Sections[I];
    if (Section.Type == (Is64 ? llvm::ELF::SHT_RELA : llvm::ELF::SHT_REL) &&
        Section.Addr == Address.first) {
      if (SectionHeader || Section.Offset != *Offset ||
          Section.Size != Size.first || Section.EntSize != sizeof(Rel))
        return fail("RELA table disagrees with section identity");
      SectionHeader = Layout->Header.ShOff + I * Layout->Header.ShEntSize;
    }
  }
  if (!SectionHeader)
    return fail("RELA section identity missing");
  auto Storage = validatePatchStorageLayout(Compiled, 4096);
  if (!Storage)
    return Storage.takeError();
  if (MetadataOffset > UINT64_MAX - (Width - 1) ||
      Added.size() > (UINT64_MAX - Size.first) / sizeof(Rel))
    return fail("relocation table size or alignment overflows");
  MetadataOffset = llvm::alignTo(MetadataOffset, Width);
  const uint64_t TableSize = Size.first + Added.size() * sizeof(Rel);
  if (!Storage->WritableOffset || MetadataOffset < Storage->ReadOnlySize ||
      !rangeInBounds(MetadataOffset, TableSize, Storage->WritableOffset))
    return fail("relocation table exceeds reserved read-only padding");
  std::vector<uint8_t> Table(Original.Raw.begin() + *Offset,
                             Original.Raw.begin() + *Offset + Size.first);
  for (const auto &Relocation : Added) {
    const auto *Bytes = reinterpret_cast<const uint8_t *>(&Relocation);
    Table.insert(Table.end(), Bytes, Bytes + sizeof(Relocation));
  }
  if constexpr (!Is64)
    for (const auto &Pointer : Pointers)
      writeLE<uint32_t>(Compiled.Bytes.data() + Pointer.FieldVA -
                            Compiled.BaseVA,
                        static_cast<uint32_t>(Pointer.Addend));
  std::copy(Table.begin(), Table.end(),
            Compiled.Bytes.begin() + MetadataOffset);
  return ELFSourceRelocationTable{Compiled.BaseVA + MetadataOffset,
                                  TableSize,
                                  Address.second,
                                  Size.second,
                                  *SectionHeader,
                                  Is64,
                                  std::move(Table)};
}

template <typename ELF>
llvm::Error installPointerRelocations(std::vector<uint8_t> &Binary,
                                      const ELFSourceRelocationTable &Table) {
  if (!Table.Size)
    return llvm::Error::success();
  auto Offset = fileOffset(Binary, Table.VA, Table.Size);
  if (!Offset)
    return Offset.takeError();
  for (uint64_t Field : {Table.AddressTagOffset, Table.SizeTagOffset})
    if (!rangeInBounds(Field, sizeof(typename ELF::Dyn), Binary.size()))
      return fail("dynamic tag exceeds candidate file");
  if (!rangeInBounds(Table.SectionHeaderOffset, sizeof(typename ELF::Shdr),
                     Binary.size()))
    return fail("relocation section header exceeds candidate file");
  typename ELF::Dyn Address, Size;
  std::memcpy(&Address, Binary.data() + Table.AddressTagOffset,
              sizeof(Address));
  std::memcpy(&Size, Binary.data() + Table.SizeTagOffset, sizeof(Size));
  constexpr bool Is64 = sizeof(typename ELF::Addr) == 8;
  if (Address.getTag() != (Is64 ? llvm::ELF::DT_RELA : llvm::ELF::DT_REL) ||
      Size.getTag() != (Is64 ? llvm::ELF::DT_RELASZ : llvm::ELF::DT_RELSZ))
    return fail("dynamic tag identity changed during installation");
  auto Section = readELFShdr(Binary.data() + Table.SectionHeaderOffset, Is64);
  if (Section.Type != (Is64 ? llvm::ELF::SHT_RELA : llvm::ELF::SHT_REL) ||
      Section.Addr != Address.getVal() || Section.Size != Size.getVal())
    return fail("relocation section identity changed during installation");
  Address.d_un.d_ptr = Table.VA;
  Size.d_un.d_val = Table.Size;
  Section.Addr = Table.VA;
  Section.Offset = *Offset;
  Section.Size = Table.Size;
  std::memcpy(Binary.data() + Table.AddressTagOffset, &Address,
              sizeof(Address));
  std::memcpy(Binary.data() + Table.SizeTagOffset, &Size, sizeof(Size));
  writeELFShdr(Binary.data() + Table.SectionHeaderOffset, Is64, Section);
  return llvm::Error::success();
}

llvm::Expected<ELFSourceRelocationTable> prepareELFPointerRelocations(
    const BinaryImage &Original, CompiledImage &Compiled,
    llvm::ArrayRef<ELFSourcePointerRelocation> Pointers,
    uint64_t MetadataOffset) {
  return Original.getPointerSize() == 8
             ? preparePointerRelocations<llvm::object::ELF64LE>(
                   Original, Compiled, Pointers, MetadataOffset)
             : preparePointerRelocations<llvm::object::ELF32LE>(
                   Original, Compiled, Pointers, MetadataOffset);
}
llvm::Error installELFSourceRelocations(std::vector<uint8_t> &Binary,
                                        const ELFSourceRelocationTable &Table) {
  return Table.Is64
             ? installPointerRelocations<llvm::object::ELF64LE>(Binary, Table)
             : installPointerRelocations<llvm::object::ELF32LE>(Binary, Table);
}
template <typename ELF>
llvm::Error verifyPointerTable(const BinaryImage &Image,
                               const ELFSourceRelocationTable &Table) {
  if (!Table.Size)
    return llvm::Error::success();
  constexpr bool Is64 = sizeof(typename ELF::Addr) == 8;
  auto L = dynamicLayout<ELF>(Image.Raw);
  if (!L)
    return L.takeError();
  const auto Address =
      L->Tags.find(Is64 ? llvm::ELF::DT_RELA : llvm::ELF::DT_REL);
  const auto Size =
      L->Tags.find(Is64 ? llvm::ELF::DT_RELASZ : llvm::ELF::DT_RELSZ);
  const auto Entry =
      L->Tags.find(Is64 ? llvm::ELF::DT_RELAENT : llvm::ELF::DT_RELENT);
  const unsigned ExpectedEntry =
      Is64 ? sizeof(typename ELF::Rela) : sizeof(typename ELF::Rel);
  if (Address == L->Tags.end() || Size == L->Tags.end() ||
      Entry == L->Tags.end() || Address->second.first != Table.VA ||
      Size->second.first != Table.Size ||
      Entry->second.first != ExpectedEntry ||
      Table.ExpectedBytes.size() != Table.Size)
    return fail("published runtime writer table differs from its receipt");
  auto Offset = fileOffset(Image.Raw, Table.VA, Table.Size);
  if (!Offset)
    return Offset.takeError();
  if (!std::equal(Table.ExpectedBytes.begin(), Table.ExpectedBytes.end(),
                  Image.Raw.begin() + *Offset))
    return fail("published runtime writer bytes differ from their receipt");
  unsigned Count = 0;
  for (const auto &S : L->Sections)
    if (S.Addr == Table.VA &&
        S.Type == (Is64 ? llvm::ELF::SHT_RELA : llvm::ELF::SHT_REL)) {
      if (S.Offset != *Offset || S.Size != Table.Size ||
          S.EntSize != ExpectedEntry)
        return fail(
            "published runtime writer section differs from dynamic table");
      ++Count;
    }
  if (Count != 1)
    return fail("runtime writer table lost its section identity");
  return llvm::Error::success();
}

llvm::Error verifyELFSourceRelocations(const BinaryImage &Image,
                                       const ELFSourceRelocationTable &Table) {
  return Table.Is64 ? verifyPointerTable<llvm::object::ELF64LE>(Image, Table)
                    : verifyPointerTable<llvm::object::ELF32LE>(Image, Table);
}

} // namespace neverd
