//===- PatchObject.cpp - Native object replacement input ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/PatchObject.h"

#include "neverd/backend/codegen/DwarfEHFrame.h"
#include "neverd/backend/codegen/ELF/ELFExceptionPatch.h"
#include "neverd/backend/codegen/ELF/ELFPatch.h"
#include "neverd/backend/codegen/ELF/ELFSectionPatch.h"
#include "neverd/backend/codegen/ELF/ELFSourceRelocations.h"
#include "neverd/backend/codegen/ELF/ELFSourceSymbols.h"
#include "neverd/backend/codegen/PatchStorage.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/object/ELFLayout.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/Object/ELF.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd {
namespace {
using ELF = llvm::object::ELF64LE;
using SectionKind = llvm::mc_rewrite::RewriteSectionKind;
constexpr uint64_t MaxImageSize = 64 * 1024 * 1024;

llvm::Error fail(const llvm::Twine &Detail) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "patch object: " + Detail);
}

struct ObjectSymbol {
  std::string Name;
  uint32_t Section = 0;
  uint64_t Offset = 0;
  uint64_t Size = 0;
  unsigned Type = 0;
  unsigned Binding = 0;
};

struct ObjectRelocation {
  size_t Section = 0;
  uint64_t Offset = 0;
  uint32_t Symbol = 0;
  uint32_t Type = 0;
  int64_t Addend = 0;
};

struct ResolvedSymbol {
  uint64_t VA = 0;
  uint32_t DynamicIndex = 0;
};

struct ObjectImage {
  // Only the common placement fields are populated. MC ownership and fixup
  // authentication fields intentionally remain empty.
  CompiledImage Storage;
  std::vector<ObjectSymbol> Symbols;
  std::vector<ObjectRelocation> Relocations;
  std::map<uint32_t, size_t> SectionIndices;
  std::map<std::string, uint32_t> NamedSymbols;
  std::map<uint32_t, uint64_t> GOT;
  std::vector<ELFSourcePointerRelocation> Pointers;
  uint64_t MetadataOffset = 0;
};

llvm::Expected<ObjectImage> readObject(llvm::StringRef Bytes) {
  if (Bytes.size() < sizeof(ELF::Ehdr) || !Bytes.starts_with("\177ELF") ||
      static_cast<uint8_t>(Bytes[llvm::ELF::EI_CLASS]) !=
          llvm::ELF::ELFCLASS64 ||
      static_cast<uint8_t>(Bytes[llvm::ELF::EI_DATA]) != llvm::ELF::ELFDATA2LSB)
    return fail("expected a native ELF64 little-endian object, not bitcode");
  auto File = llvm::object::ELFFile<ELF>::create(Bytes);
  if (!File)
    return File.takeError();
  const auto &Header = File->getHeader();
  if (Header.e_type != llvm::ELF::ET_REL ||
      Header.e_machine != llvm::ELF::EM_X86_64 || Header.e_flags != 0 ||
      Header.e_phnum != 0 || Header.e_version != llvm::ELF::EV_CURRENT ||
      (Header.e_ident[llvm::ELF::EI_OSABI] != llvm::ELF::ELFOSABI_NONE &&
       Header.e_ident[llvm::ELF::EI_OSABI] != llvm::ELF::ELFOSABI_GNU))
    return fail("only ELF x64 relocatable objects are admitted");
  auto Sections = File->sections();
  if (!Sections)
    return Sections.takeError();
  if (Sections->empty() || Sections->size() > 65536)
    return fail("invalid section count");
  ObjectImage Result;
  Result.Storage.InputKind = RewriteInputKind::ReplacementObject;
  Result.Storage.TargetArch = Arch::X64;
  Result.Storage.Format = BinaryFormat::ELF;
  Result.Storage.TargetTriple = "x86_64-unknown-linux-gnu";
  Result.Storage.PointerWidth = 8;
  uint64_t AllocatedBytes = 0;
  std::optional<uint32_t> Symtab;
  for (uint32_t I = 0; I < Sections->size(); ++I) {
    const auto &Section = (*Sections)[I];
    auto Name = File->getSectionName(Section);
    if (!Name)
      return Name.takeError();
    if (Name->starts_with(".gnu.lto_") || Name->starts_with(".llvm.lto") ||
        Name->starts_with(".llvmbc"))
      return fail("LTO payloads are not native code");
    if (Section.sh_type == llvm::ELF::SHT_SYMTAB) {
      if (Symtab)
        return fail("multiple symbol tables");
      Symtab = I;
    }
    if (*Name == ".note.GNU-stack" &&
        (Section.sh_flags & llvm::ELF::SHF_EXECINSTR))
      return fail("executable-stack requirement is unsupported");
    if (!(Section.sh_flags & llvm::ELF::SHF_ALLOC))
      continue;
    if (*Name == ".init" || *Name == ".fini" || Name->starts_with(".ctors") ||
        Name->starts_with(".dtors"))
      return fail("runtime initialization/finalization is unsupported: " +
                  *Name);
    const uint64_t Flags = Section.sh_flags;
    constexpr uint64_t AllowedFlags =
        llvm::ELF::SHF_ALLOC | llvm::ELF::SHF_WRITE | llvm::ELF::SHF_EXECINSTR |
        llvm::ELF::SHF_MERGE | llvm::ELF::SHF_STRINGS | llvm::ELF::SHF_GROUP |
        llvm::ELF::SHF_GNU_RETAIN | llvm::ELF::SHF_LINK_ORDER;
    const bool Property = *Name == ".note.gnu.property" &&
                          Section.sh_type == llvm::ELF::SHT_NOTE &&
                          Flags == llvm::ELF::SHF_ALLOC;
    if (Property) {
      auto Note = File->getSectionContents(Section);
      if (!Note)
        return Note.takeError();
      auto Features = readELFX64PatchPropertyNote(*Note);
      if (!Features)
        return Features.takeError();
    }
    if ((Flags & llvm::ELF::SHF_LINK_ORDER) &&
        (Section.sh_link == 0 || Section.sh_link >= Sections->size() ||
         !((*Sections)[Section.sh_link].sh_flags & llvm::ELF::SHF_ALLOC)))
      return fail("invalid link-order section dependency");
    if ((Flags & ~AllowedFlags) ||
        (Section.sh_type != llvm::ELF::SHT_PROGBITS &&
         Section.sh_type != llvm::ELF::SHT_NOBITS &&
         Section.sh_type != llvm::ELF::SHT_X86_64_UNWIND && !Property) ||
        ((Flags & llvm::ELF::SHF_WRITE) && (Flags & llvm::ELF::SHF_EXECINSTR)))
      return fail("unsupported allocated section or permissions: " + *Name);
    const uint64_t Alignment = std::max<uint64_t>(Section.sh_addralign, 1);
    if (!llvm::isPowerOf2_64(Alignment) || Alignment > 1024 * 1024 ||
        Section.sh_size > MaxImageSize || Name->empty())
      return fail("invalid allocated section: " + *Name);
    if (Section.sh_size > MaxImageSize - AllocatedBytes)
      return fail("total allocated storage exceeds the profile limit");
    AllocatedBytes += Section.sh_size;
    if (*Name == ".got.source")
      return fail("reserved synthetic GOT section name");
    CompiledSection Item;
    Item.Name = Name->str();
    Item.Alignment = Alignment;
    Item.Size = Section.sh_size;
    Item.Kind = (Flags & llvm::ELF::SHF_EXECINSTR) ? SectionKind::Code
                : Section.sh_type == llvm::ELF::SHT_NOBITS
                    ? SectionKind::UninitializedData
                : (Flags & llvm::ELF::SHF_WRITE) ? SectionKind::WritableData
                                                 : SectionKind::ReadOnlyData;
    if (Section.sh_type == llvm::ELF::SHT_NOBITS) {
      if (!(Flags & llvm::ELF::SHF_WRITE))
        return fail("non-writable zero-fill storage is unsupported");
      Item.ExternalBytes.resize(Item.Size, 0);
    } else {
      auto Contents = File->getSectionContents(Section);
      if (!Contents)
        return Contents.takeError();
      Item.ExternalBytes.assign(Contents->begin(), Contents->end());
    }
    Result.SectionIndices[I] = Result.Storage.Sections.size();
    Result.Storage.Sections.push_back(std::move(Item));
  }
  if (!Symtab)
    return fail("a symbol table is required");
  auto Symbols = File->symbols(&(*Sections)[*Symtab]);
  if (!Symbols)
    return Symbols.takeError();
  auto Names = File->getStringTableForSymtab((*Sections)[*Symtab]);
  if (!Names)
    return Names.takeError();
  if (Symbols->size() > 1024 * 1024)
    return fail("excessive symbol count");
  for (const auto &Symbol : *Symbols) {
    auto Name = Symbol.getName(*Names);
    if (!Name)
      return Name.takeError();
    ObjectSymbol Item{Name->str(),    Symbol.st_shndx,  Symbol.st_value,
                      Symbol.st_size, Symbol.getType(), Symbol.getBinding()};
    if (Item.Type == llvm::ELF::STT_TLS ||
        Item.Type == llvm::ELF::STT_GNU_IFUNC ||
        Item.Section == llvm::ELF::SHN_COMMON ||
        Item.Section == llvm::ELF::SHN_XINDEX ||
        (Item.Binding != llvm::ELF::STB_LOCAL &&
         Item.Binding != llvm::ELF::STB_GLOBAL &&
         Item.Binding != llvm::ELF::STB_WEAK))
      return fail("unsupported symbol: " + Item.Name);
    if (Item.Section != llvm::ELF::SHN_UNDEF &&
        Item.Section != llvm::ELF::SHN_ABS) {
      if (Item.Section >= Sections->size() ||
          !rangeInBounds(Item.Offset, Item.Size,
                         (*Sections)[Item.Section].sh_size))
        return fail("symbol exceeds its section: " + Item.Name);
    }
    if (!Item.Name.empty() && Item.Type != llvm::ELF::STT_FILE &&
        Item.Type != llvm::ELF::STT_SECTION &&
        !Result.NamedSymbols.emplace(Item.Name, Result.Symbols.size()).second)
      return fail("duplicate symbol: " + Item.Name);
    Result.Symbols.push_back(std::move(Item));
  }
  // One object is installed in its entirety: COMDAT members are retained
  // together, without archive selection or cross-object ODR coalescing.
  std::set<uint32_t> GroupMembers;
  std::set<std::string> GroupSignatures;
  for (uint32_t I = 0; I < Sections->size(); ++I) {
    const auto &Section = (*Sections)[I];
    if (Section.sh_type == llvm::ELF::SHT_GROUP) {
      auto Contents = File->getSectionContents(Section);
      if (!Contents)
        return Contents.takeError();
      if (Section.sh_link != *Symtab ||
          Section.sh_info >= Result.Symbols.size() || Contents->size() < 8 ||
          Contents->size() % 4 ||
          readLE<uint32_t>(Contents->data()) != llvm::ELF::GRP_COMDAT)
        return fail("malformed or unsupported section group");
      const auto &Signature = Result.Symbols[Section.sh_info].Name;
      if (Signature.empty() || !GroupSignatures.insert(Signature).second)
        return fail("ambiguous COMDAT signature");
      for (size_t O = 4; O < Contents->size(); O += 4) {
        const auto Member = readLE<uint32_t>(Contents->data() + O);
        if (!Member || Member >= Sections->size() || Member == I ||
            !((*Sections)[Member].sh_flags & llvm::ELF::SHF_GROUP) ||
            !GroupMembers.insert(Member).second)
          return fail("invalid or multiply owned COMDAT member");
      }
    }
    if (Section.sh_type != llvm::ELF::SHT_RELA &&
        Section.sh_type != llvm::ELF::SHT_REL)
      continue;
    if (Section.sh_info >= Sections->size())
      return fail("invalid relocation target section");
    auto Target = Result.SectionIndices.find(Section.sh_info);
    if (Target == Result.SectionIndices.end())
      continue; // Debug relocations do not belong to the loaded image.
    if (Section.sh_type != llvm::ELF::SHT_RELA || Section.sh_link != *Symtab)
      return fail(
          "allocated relocations must use the unique RELA symbol table");
    auto Relocations = File->relas(Section);
    if (!Relocations)
      return Relocations.takeError();
    for (const auto &Relocation : *Relocations) {
      const uint32_t Symbol = Relocation.getSymbol(false);
      const uint32_t Type = Relocation.getType(false);
      if (Symbol >= Result.Symbols.size())
        return fail("relocation has invalid symbol index");
      Result.Relocations.push_back({Target->second, Relocation.r_offset, Symbol,
                                    Type, Relocation.r_addend});
      if (Type == llvm::ELF::R_X86_64_GOTPCREL ||
          Type == llvm::ELF::R_X86_64_GOTPCRELX ||
          Type == llvm::ELF::R_X86_64_REX_GOTPCRELX)
        Result.GOT.emplace(Symbol, 0);
    }
  }
  for (uint32_t I = 0; I < Sections->size(); ++I)
    if (((*Sections)[I].sh_flags & llvm::ELF::SHF_GROUP) &&
        !GroupMembers.count(I))
      return fail("section group membership is missing");
  return Result;
}

llvm::Error placeObject(ObjectImage &Object, const BinaryImage &Original,
                        uint64_t BaseVA, const ELFEHFrameRegion &EH) {
  auto &Image = Object.Storage;
  Image.BaseVA = BaseVA;
  if (!BaseVA || BaseVA > UINT64_MAX - MaxImageSize)
    return fail("invalid placement base");
  if (!Object.GOT.empty()) {
    CompiledSection GOT;
    GOT.Name = ".got.source";
    GOT.Kind = SectionKind::WritableData;
    GOT.Size = Object.GOT.size() * 8;
    GOT.Alignment = 8;
    GOT.ExternalBytes.resize(GOT.Size, 0);
    Image.Sections.push_back(std::move(GOT));
  }
  uint64_t Cursor = BaseVA;
  auto Place = [&](CompiledSection &Section) -> llvm::Error {
    Cursor = llvm::alignTo(Cursor, Section.Alignment);
    if (Cursor < BaseVA || Cursor - BaseVA > MaxImageSize ||
        Section.Size > MaxImageSize - (Cursor - BaseVA))
      return fail("object placement exceeds the profile limit");
    Section.VA = Cursor;
    Section.Offset = Cursor - BaseVA;
    Cursor += Section.Size;
    return llvm::Error::success();
  };
  for (auto &Section : Image.Sections)
    if (Section.Kind == SectionKind::Code)
      if (auto Error = Place(Section))
        return Error;
  for (auto &Section : Image.Sections)
    if (Section.Kind == SectionKind::ReadOnlyData)
      if (auto Error = Place(Section))
        return Error;
  Object.MetadataOffset = llvm::alignTo(Cursor - BaseVA, 8);
  // Reserve enough for the old and new search/RELA tables before deciding RW
  // addresses. The actual tables are rebuilt from their authenticated inputs.
  uint64_t Reserve = EH.HdrSize + Object.Symbols.size() * 8 + 64 +
                     (Object.Relocations.size() + Object.GOT.size()) * 24;
  forEachELFShdr(Original.Raw.data(), Original.Raw.size(),
                 [&](const ELFShdrFields &Section, uint16_t) {
                   if (Section.Type == llvm::ELF::SHT_RELA)
                     Reserve += Section.Size;
                 });
  if (Reserve > MaxImageSize || Object.MetadataOffset > MaxImageSize - Reserve)
    return fail("object metadata exceeds the profile limit");
  Cursor = BaseVA + Object.MetadataOffset + Reserve;
  const bool HasWritable = std::any_of(
      Image.Sections.begin(), Image.Sections.end(), [](const auto &Section) {
        return Section.Size && (Section.Kind == SectionKind::WritableData ||
                                Section.Kind == SectionKind::UninitializedData);
      });
  if (HasWritable)
    Cursor = llvm::alignTo(Cursor, 4096);
  for (auto &Section : Image.Sections)
    if (Section.Kind == SectionKind::WritableData ||
        Section.Kind == SectionKind::UninitializedData)
      if (auto Error = Place(Section))
        return Error;
  if (Cursor - BaseVA > MaxImageSize)
    return fail("object image is too large");
  Image.Bytes.resize(Cursor - BaseVA, 0);
  for (auto &Section : Image.Sections) {
    std::copy(Section.ExternalBytes.begin(), Section.ExternalBytes.end(),
              Image.Bytes.begin() + Section.Offset);
    Section.ExternalBytes.clear();
  }
  if (!Object.GOT.empty()) {
    uint64_t Slot = Image.Sections.back().VA;
    for (auto &[Symbol, VA] : Object.GOT) {
      VA = Slot;
      Slot += 8;
    }
  }
  for (const auto &[Name, Index] : Object.NamedSymbols) {
    const auto &Symbol = Object.Symbols[Index];
    auto Section = Object.SectionIndices.find(Symbol.Section);
    if (Section != Object.SectionIndices.end())
      Image.SymbolAddrs[Name] =
          Image.Sections[Section->second].VA + Symbol.Offset;
  }
  Image.Success = true;
  auto Layout = validatePatchStorageLayout(Image, 4096);
  return Layout ? llvm::Error::success() : Layout.takeError();
}

llvm::Expected<ResolvedSymbol> resolveObjectSymbol(
    const ObjectImage &Object, uint32_t Index, const BinaryImage &Original,
    const std::map<std::string, SourcePatchBinding> &Bindings, bool Callable) {
  const auto &Symbol = Object.Symbols[Index];
  if (Symbol.Section != llvm::ELF::SHN_UNDEF) {
    auto Section = Object.SectionIndices.find(Symbol.Section);
    if (Section == Object.SectionIndices.end())
      return fail("reference to non-allocated or absolute symbol: " +
                  Symbol.Name);
    const auto &Storage = Object.Storage.Sections[Section->second];
    if (Callable &&
        (Storage.Kind != SectionKind::Code || Symbol.Offset >= Storage.Size))
      return fail("callable symbol does not name code: " + Symbol.Name);
    return ResolvedSymbol{Storage.VA + Symbol.Offset, 0};
  }
  if (auto Binding = Bindings.find(Symbol.Name); Binding != Bindings.end()) {
    if (Callable && Binding->second.Kind != SourcePatchBindingKind::Function)
      return fail("data binding used as a callable target: " + Symbol.Name);
    return ResolvedSymbol{Binding->second.OriginalVA, 0};
  }
  if (Symbol.Binding == llvm::ELF::STB_WEAK)
    return fail("unresolved weak symbol: " + Symbol.Name);
  if (!Callable) {
    auto Dynamic = findELFSourceDynamicSymbol(Original, Symbol.Name);
    if (!Dynamic)
      return Dynamic.takeError();
    if (*Dynamic)
      return ResolvedSymbol{0, **Dynamic};
    return fail("external data/address requires an explicit binding or "
                "existing import: " +
                Symbol.Name);
  }
  std::set<uint64_t> Addresses;
  for (const auto &Candidate : Original.Symbols)
    if (Candidate.Name == Symbol.Name && Candidate.IsFunc &&
        Original.hasAuthenticatedFunctionEntryAt(Candidate.Addr))
      Addresses.insert(Candidate.Addr);
  for (const auto &Export : Original.Exports)
    if (Export.Name == Symbol.Name &&
        Original.hasAuthenticatedFunctionEntryAt(Export.Addr))
      Addresses.insert(Export.Addr);
  for (const auto &[VA, Import] : Original.ImportStubIndices)
    if (Import < Original.Imports.size() &&
        Original.Imports[Import].Name == Symbol.Name &&
        Original.findImportStubAt(VA) && Original.isCodeAddress(VA))
      Addresses.insert(VA);
  if (Addresses.size() != 1)
    return fail("missing or ambiguous original callable symbol: " +
                Symbol.Name);
  return ResolvedSymbol{*Addresses.begin(), 0};
}

llvm::Error
relocateObject(ObjectImage &Object, const BinaryImage &Original,
               const std::map<std::string, SourcePatchBinding> &Bindings) {
  auto &Image = Object.Storage;
  for (const auto &[Symbol, Field] : Object.GOT) {
    auto Target =
        resolveObjectSymbol(Object, Symbol, Original, Bindings, false);
    if (!Target)
      return Target.takeError();
    Object.Pointers.push_back(
        {Field, Target->DynamicIndex, static_cast<int64_t>(Target->VA)});
    writeLE<uint64_t>(Image.Bytes.data() + Field - Image.BaseVA, Target->VA);
  }
  std::map<uint64_t, uint64_t> Written;
  for (const auto &Relocation : Object.Relocations) {
    if (Relocation.Type == llvm::ELF::R_X86_64_NONE)
      continue;
    const auto &Section = Image.Sections[Relocation.Section];
    const bool Absolute = Relocation.Type == llvm::ELF::R_X86_64_64;
    const bool PCRel = Relocation.Type == llvm::ELF::R_X86_64_PC32 ||
                       Relocation.Type == llvm::ELF::R_X86_64_PLT32;
    const bool GOT = Relocation.Type == llvm::ELF::R_X86_64_GOTPCREL ||
                     Relocation.Type == llvm::ELF::R_X86_64_GOTPCRELX ||
                     Relocation.Type == llvm::ELF::R_X86_64_REX_GOTPCRELX;
    const uint64_t Width = Absolute ? 8 : 4;
    if (!Absolute && !PCRel && !GOT)
      return fail("unsupported x64 relocation " + llvm::Twine(Relocation.Type));
    if (!rangeInBounds(Relocation.Offset, Width, Section.Size))
      return fail("relocation exceeds its owning section");
    const uint64_t Field = Section.VA + Relocation.Offset;
    auto Next = Written.lower_bound(Field);
    if ((Next != Written.end() && Next->first - Field < Width) ||
        (Next != Written.begin() &&
         Field - std::prev(Next)->first < std::prev(Next)->second))
      return fail("overlapping relocation writers");
    Written[Field] = Width;
    auto Target =
        GOT ? llvm::Expected<ResolvedSymbol>(
                  ResolvedSymbol{Object.GOT.at(Relocation.Symbol), 0})
            : resolveObjectSymbol(Object, Relocation.Symbol, Original, Bindings,
                                  Relocation.Type == llvm::ELF::R_X86_64_PLT32);
    if (!Target)
      return Target.takeError();
    uint8_t *Output = Image.Bytes.data() + Section.Offset + Relocation.Offset;
    if (Absolute) {
      if (Section.Kind != SectionKind::WritableData &&
          Section.Kind != SectionKind::UninitializedData)
        return fail("absolute pointer needs writable relocation storage");
      const uint64_t Value =
          Target->VA + static_cast<uint64_t>(Relocation.Addend);
      Object.Pointers.push_back({Field, Target->DynamicIndex,
                                 Target->DynamicIndex
                                     ? Relocation.Addend
                                     : static_cast<int64_t>(Value)});
      writeLE<uint64_t>(Output, Value);
    } else {
      if (Target->DynamicIndex)
        return fail(
            "direct PC-relative reference to dynamic data requires binding");
      // APInt checks the full signed expression before narrowing to rel32.
      llvm::APInt Delta(128, Target->VA);
      Delta += llvm::APInt(128, static_cast<uint64_t>(Relocation.Addend), true);
      Delta -= llvm::APInt(128, Field);
      if (!Delta.isSignedIntN(32))
        return fail(
            "PC-relative relocation is outside the signed 32-bit range");
      writeLE<uint32_t>(
          Output, static_cast<uint32_t>(Delta.getLoBits(32).getZExtValue()));
    }
  }
  return llvm::Error::success();
}

llvm::Expected<std::vector<uint64_t>>
validateObjectUnwind(const ObjectImage &Object) {
  const auto &Image = Object.Storage;
  const CompiledSection *EH = nullptr;
  std::map<uint64_t, uint64_t> Functions;
  for (const auto &Symbol : Object.Symbols) {
    if (Symbol.Type != llvm::ELF::STT_FUNC ||
        Symbol.Section == llvm::ELF::SHN_UNDEF)
      continue;
    auto Section = Object.SectionIndices.find(Symbol.Section);
    if (Section == Object.SectionIndices.end() || Symbol.Size == 0 ||
        Image.Sections[Section->second].Kind != SectionKind::Code)
      return fail("function requires a bounded executable symbol: " +
                  Symbol.Name);
    const uint64_t Begin = Image.Sections[Section->second].VA + Symbol.Offset;
    auto [It, Inserted] = Functions.emplace(Begin, Begin + Symbol.Size);
    if (!Inserted && It->second != Begin + Symbol.Size)
      return fail("aliased functions disagree about their range");
  }
  for (const auto &Section : Image.Sections)
    if (Section.Name == ".eh_frame") {
      if (EH)
        return fail("duplicate object unwind section");
      EH = &Section;
    }
  if (!EH || Functions.empty())
    return fail(
        "object profile requires compiler-generated CFI for every function");
  auto Records = decodeDwarfEHFrameRecords(
      llvm::ArrayRef<uint8_t>(Image.Bytes).slice(EH->Offset, EH->Size), EH->VA,
      true);
  if (!Records)
    return Records.takeError();
  std::set<uint64_t> Covered;
  for (const auto &Record : *Records) {
    auto Function = Functions.find(Record.BeginVA);
    if (Function == Functions.end() || Function->second != Record.EndVA ||
        !Covered.insert(Record.BeginVA).second)
      return fail("FDE range does not match an exact object function range");
  }
  if (Covered.size() != Functions.size())
    return fail("not every object function has registered unwind coverage");
  return std::vector<uint64_t>(Covered.begin(), Covered.end());
}

} // namespace

llvm::Expected<PatchResult> installObjectPatch(
    llvm::StringRef Bytes, const BinaryImage &Original,
    llvm::ArrayRef<SourcePatchMapping> Mappings,
    llvm::ArrayRef<SourcePatchBinding> RequestedBindings,
    const std::filesystem::path &Output,
    llvm::function_ref<llvm::Error(const PatchResult &)> PrepareReceipt) {
  if (Original.Format != BinaryFormat::ELF || Original.Arch != Arch::X64)
    return fail("native object installation currently requires ELF x64");
  if (Mappings.empty())
    return fail("explicit entry and original address are required");
  if (auto Error = validateELFSourceFeatures(Original))
    return std::move(Error);
  auto Object = readObject(Bytes);
  if (!Object)
    return Object.takeError();
  BinaryImage References = Original;
  scanImportThunks(References);
  std::set<std::string> Entries;
  std::set<va_t> Addresses;
  for (const auto &Mapping : Mappings) {
    auto Symbol = Object->NamedSymbols.find(Mapping.Symbol);
    if (Mapping.Symbol.empty() || Symbol == Object->NamedSymbols.end() ||
        !Entries.insert(Mapping.Symbol).second ||
        !Addresses.insert(Mapping.OriginalVA).second)
      return fail("missing or duplicate explicit object entry");
    const auto &Definition = Object->Symbols[Symbol->second];
    if (Definition.Type != llvm::ELF::STT_FUNC ||
        Definition.Section == llvm::ELF::SHN_UNDEF ||
        !Original.hasAuthenticatedFunctionEntryAt(Mapping.OriginalVA) ||
        !sourceEntryCanHostTrampoline(Original, Mapping.OriginalVA))
      return fail(
          "entry lacks a bounded definition or replaceable original target");
  }
  std::map<std::string, SourcePatchBinding> Bindings;
  std::set<va_t> BoundAddresses;
  for (const auto &Binding : RequestedBindings) {
    auto Symbol = Object->NamedSymbols.find(Binding.Symbol);
    if (Symbol == Object->NamedSymbols.end() ||
        Object->Symbols[Symbol->second].Section != llvm::ELF::SHN_UNDEF ||
        !Bindings.emplace(Binding.Symbol, Binding).second ||
        !BoundAddresses.insert(Binding.OriginalVA).second)
      return fail("binding requires a unique undefined object symbol");
    if (auto Error = validateSourcePatchBinding(Binding, References, Mappings))
      return std::move(Error);
    const auto &Declaration = Object->Symbols[Symbol->second];
    const auto ExpectedType = Binding.Kind == SourcePatchBindingKind::Function
                                  ? llvm::ELF::STT_FUNC
                                  : llvm::ELF::STT_OBJECT;
    if (Declaration.Type != llvm::ELF::STT_NOTYPE &&
        Declaration.Type != ExpectedType)
      return fail("binding disagrees with object symbol type: " +
                  Binding.Symbol);
    if (Binding.Kind != SourcePatchBindingKind::Function &&
        Declaration.Size > Binding.Size)
      return fail("binding is smaller than the object declaration: " +
                  Binding.Symbol);
  }
  auto EH = findELFEHFrameRegion(Original.Raw);
  if (!EH || !EH->HasHdr)
    return fail(
        "original requires an existing registered ELF unwind search table");
  ELFPatcher Patcher;
  Patcher.setInputKind(RewriteInputKind::ReplacementObject);
  const uint64_t Base = Patcher.plannedExecSegmentVA(Original.Raw, Arch::X64);
  if (auto Error = placeObject(*Object, Original, Base, *EH))
    return std::move(Error);
  if (auto Error = relocateObject(*Object, References, Bindings))
    return std::move(Error);
  auto Functions = validateObjectUnwind(*Object);
  if (!Functions)
    return Functions.takeError();
  auto &Image = Object->Storage;
  const uint64_t HdrVA = Image.BaseVA + Object->MetadataOffset;
  auto Hdr =
      buildRelocatedELFEHFrameHdr(Original.Raw, *EH, Image, *Functions, HdrVA);
  if (!Hdr)
    return Hdr.takeError();
  auto Storage = validatePatchStorageLayout(Image, 4096);
  if (!Storage)
    return Storage.takeError();
  const uint64_t Limit =
      Storage->WritableOffset ? Storage->WritableOffset : Image.Bytes.size();
  if (!rangeInBounds(Object->MetadataOffset, Hdr->size(), Limit))
    return fail("unwind search table exceeds its read-only reservation");
  std::copy(Hdr->begin(), Hdr->end(),
            Image.Bytes.begin() + Object->MetadataOffset);
  auto Dynamic =
      prepareELFPointerRelocations(References, Image, Object->Pointers,
                                   Object->MetadataOffset + Hdr->size());
  if (!Dynamic)
    return Dynamic.takeError();
  return publishSourcePatch(
      Original, Mappings, Output,
      [&](const std::filesystem::path &Input,
          const std::filesystem::path &Candidate)
          -> llvm::Expected<PatchResult> {
        std::string Failure;
        auto Result = BinaryPatcher::readPatchWrite(
            Input, Candidate, true, "object_patch",
            [&](std::vector<uint8_t> &Binary, PatchResult &Result) {
              auto Reject = [&](llvm::Error Error) {
                Failure = llvm::toString(std::move(Error));
                return false;
              };
              const auto Writable =
                  Storage->WritableOffset
                      ? std::optional<uint64_t>(Image.BaseVA +
                                                Storage->WritableOffset)
                      : std::nullopt;
              if (Patcher.appendPatchSegments(Binary, Image.Bytes, Arch::X64,
                                              Writable) != Image.BaseVA)
                return Reject(fail("placement changed during installation"));
              if (auto Error =
                      retargetELFEHFrameHdr(Binary, HdrVA, Hdr->size()))
                return Reject(std::move(Error));
              if (auto Error = installELFSourceRelocations(Binary, *Dynamic))
                return Reject(std::move(Error));
              if (auto Error = registerELFPatchSections(Binary, Image.Sections))
                return Reject(std::move(Error));
              for (const auto &Mapping : Mappings) {
                const uint64_t Owner = Image.SymbolAddrs.at(Mapping.Symbol);
                const uint64_t Offset =
                    Original.vaToFileOffset(Mapping.OriginalVA);
                if (!getTargetCodegenInfo(Arch::X64).writeTrampoline(
                        Binary, Offset, Owner, Mapping.OriginalVA, 5))
                  return Reject(
                      fail("object entry is outside trampoline range"));
                Result.PatchedFunctions.push_back({Mapping.Symbol,
                                                   Mapping.OriginalVA, Owner,
                                                   Mapping.Symbol});
                Result.PatchedOriginalEntries.push_back(Mapping.OriginalVA);
              }
              std::sort(Result.PatchedOriginalEntries.begin(),
                        Result.PatchedOriginalEntries.end());
              Result.TrampolineCount = Mappings.size();
              Result.CodeSize = Image.Bytes.size();
              return true;
            });
        if (!Result.Success)
          return fail(Failure.empty() ? "candidate write failed" : Failure);
        return Result;
      },
      PrepareReceipt);
}

} // namespace neverd
