//===- ELFSourceDependencies.cpp - Source patch support ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve ELF loader tables while adding imports.
//===----------------------------------------------------------------------===//

#include "ELFSourceDependencies.h"

#include "neverd/backend/Limits.h"
#include "neverd/backend/codegen/ELF/ELFPatch.h"
#include "neverd/object/ELFLayout.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

#include <map>
#include <set>

namespace neverd {
namespace {
llvm::Error fail(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "ELF dependency: " + Message);
}

template <class ELF> struct Layout {
  ELFHeaderInfo Header;
  ELFPhdrFields Dynamic;
  std::vector<typename ELF::Dyn> Entries;
  std::map<int64_t, uint64_t> Tags;
  std::vector<typename ELF::Shdr> Sections;
  size_t Symbols = 0, Strings = 0, Versions = 0, Relocations = 0, Hash = 0;
};

template <class ELF>
llvm::Expected<Layout<ELF>> readLayout(const BinaryImage &Image) {
  using namespace llvm::ELF;
  Layout<ELF> L;
  if (!validateELFHeaderTables(Image.Raw.data(), Image.Raw.size(), L.Header) ||
      L.Header.Is64 != (sizeof(typename ELF::Addr) == 8))
    return fail("a complete little-endian image is required");
  unsigned Count = 0;
  forEachELFPhdr(Image.Raw.data(), Image.Raw.size(),
                 [&](const ELFPhdrFields &P, const uint8_t *, bool) {
                   if (P.Type == PT_DYNAMIC) {
                     L.Dynamic = P;
                     ++Count;
                   }
                 });
  if (Count != 1 || !L.Dynamic.FileSz ||
      L.Dynamic.FileSz >
          ::neverd::limits::backend::kMaxELFDependencyDynamicBytes ||
      L.Dynamic.FileSz % sizeof(typename ELF::Dyn) ||
      !rangeInBounds(L.Dynamic.Offset, L.Dynamic.FileSz, Image.Raw.size()))
    return fail("one existing dynamic loader table is required");
  const auto *Mapped = Image.readVA(L.Dynamic.VAddr, L.Dynamic.FileSz);
  if (!Mapped || !std::equal(Mapped, Mapped + L.Dynamic.FileSz,
                             Image.Raw.begin() + L.Dynamic.Offset))
    return fail("dynamic file and mapped bytes disagree");
  bool End = false;
  for (uint64_t N = 0; N < L.Dynamic.FileSz; N += sizeof(typename ELF::Dyn)) {
    typename ELF::Dyn D;
    std::memcpy(&D, Image.Raw.data() + L.Dynamic.Offset + N, sizeof(D));
    if (D.getTag() == DT_NULL) {
      End = true;
      break;
    }
    if (D.getTag() == DT_NEEDED) {
      L.Entries.push_back(D);
      continue;
    }
    if (!L.Tags.emplace(D.getTag(), D.getVal()).second)
      return fail("duplicate dynamic tag");
    // These formats carry additional symbol-index/relocation contracts.
    // Do not reinterpret them as ordinary named eager imports.
    if (D.getTag() > DT_FLAGS) {
      switch (D.getTag()) {
      case DT_PREINIT_ARRAY:
      case DT_PREINIT_ARRAYSZ:
      case DT_RELR:
      case DT_RELRSZ:
      case DT_RELRENT:
      case DT_GNU_HASH:
      case DT_VERSYM:
      case DT_VERDEF:
      case DT_VERDEFNUM:
      case DT_VERNEED:
      case DT_VERNEEDNUM:
      case DT_RELACOUNT:
      case DT_RELCOUNT:
      case DT_FLAGS_1:
        break;
      default:
        return fail("unmodeled dynamic symbol/relocation extension");
      }
    }
    L.Entries.push_back(D);
  }
  if (!End || L.Tags[DT_SYMENT] != sizeof(typename ELF::Sym) ||
      !L.Tags[DT_SYMTAB] || !L.Tags[DT_STRTAB] || !L.Tags[DT_STRSZ])
    return fail("incomplete dynamic symbol metadata");
  for (unsigned I = 0; I < L.Header.ShNum; ++I) {
    typename ELF::Shdr S;
    std::memcpy(&S, Image.Raw.data() + L.Header.ShOff + uint64_t(I) * sizeof(S),
                sizeof(S));
    L.Sections.push_back(S);
    if (S.sh_type == SHT_DYNSYM && S.sh_addr == L.Tags[DT_SYMTAB]) {
      if (L.Symbols)
        return fail("ambiguous dynamic symbol section");
      L.Symbols = I;
    }
  }
  if (!L.Symbols)
    return fail("dynamic symbols require an exact section identity");
  const auto &Symbols = L.Sections[L.Symbols];
  L.Strings = Symbols.sh_link;
  if (Symbols.sh_entsize != sizeof(typename ELF::Sym) || !Symbols.sh_size ||
      Symbols.sh_size % sizeof(typename ELF::Sym) ||
      Symbols.sh_size / sizeof(typename ELF::Sym) >
          ::neverd::limits::backend::kMaxELFDependencySymbols ||
      L.Strings >= L.Sections.size())
    return fail("invalid dynamic symbol extent");
  const auto &Strings = L.Sections[L.Strings];
  if (Strings.sh_type != SHT_STRTAB || Strings.sh_addr != L.Tags[DT_STRTAB] ||
      Strings.sh_size != L.Tags[DT_STRSZ] ||
      Strings.sh_size > ::neverd::limits::backend::
                            kELFSourceDependenciesReadLayoutStringsShSizeLimit)
    return fail("dynamic string section disagrees with loader metadata");
  for (const auto *S : {&Symbols, &Strings}) {
    const auto *B = Image.readVA(S->sh_addr, S->sh_size);
    if (!B || !rangeInBounds(S->sh_offset, S->sh_size, Image.Raw.size()) ||
        !std::equal(B, B + S->sh_size, Image.Raw.begin() + S->sh_offset))
      return fail("dynamic section has no exact file-backed bytes");
  }
  for (size_t I = 1; I < L.Sections.size(); ++I) {
    const auto &S = L.Sections[I];
    if (S.sh_type == SHT_GNU_versym && S.sh_addr == L.Tags[DT_VERSYM])
      L.Versions = I;
    if ((S.sh_type == SHT_HASH && S.sh_addr == L.Tags[DT_HASH]) ||
        (!L.Hash && S.sh_type == SHT_GNU_HASH &&
         S.sh_addr == L.Tags[DT_GNU_HASH]))
      L.Hash = I;
  }
  if (L.Tags[DT_VERSYM] && !L.Versions)
    return fail("symbol versions have no exact section identity");
  const bool Is64 = sizeof(typename ELF::Addr) == 8;
  const uint64_t Prefix = L.Tags[Is64 ? DT_RELACOUNT : DT_RELCOUNT];
  using Rel = std::conditional_t<sizeof(typename ELF::Addr) == 8,
                                 typename ELF::Rela, typename ELF::Rel>;
  const uint64_t Size = L.Tags[Is64 ? DT_RELASZ : DT_RELSZ];
  if (Prefix > Size / sizeof(Rel) || (Prefix && Size % sizeof(Rel)))
    return fail("relative relocation prefix exceeds its table");
  if (Prefix) {
    const auto *Bytes = Image.readVA(L.Tags[Is64 ? DT_RELA : DT_REL], Size);
    if (!Bytes)
      return fail("relative relocation prefix is not mapped");
    const uint32_t Type = Image.Arch == Arch::X86   ? R_386_RELATIVE
                          : Image.Arch == Arch::X64 ? R_X86_64_RELATIVE
                          : Image.Arch == Arch::ARM ? R_ARM_RELATIVE
                                                    : R_AARCH64_RELATIVE;
    for (uint64_t I = 0; I < Prefix; ++I) {
      Rel R;
      std::memcpy(&R, Bytes + I * sizeof(Rel), sizeof(Rel));
      if (R.getType(false) != Type || R.getSymbol(false))
        return fail("relative relocation prefix contains a symbol writer");
    }
  }
  return L;
}

template <class ELF>
llvm::Expected<std::vector<uint8_t>>
append(const BinaryImage &Original, std::vector<SourcePatchImport> &Imports) {
  using namespace llvm::ELF;
  constexpr bool Rela = sizeof(typename ELF::Addr) == 8;
  constexpr int64_t RelTag = Rela ? DT_RELA : DT_REL;
  constexpr int64_t SizeTag = Rela ? DT_RELASZ : DT_RELSZ;
  constexpr int64_t EntTag = Rela ? DT_RELAENT : DT_RELENT;
  using Rel = std::conditional_t<Rela, typename ELF::Rela, typename ELF::Rel>;
  auto Read = readLayout<ELF>(Original);
  if (!Read)
    return Read.takeError();
  auto L = std::move(*Read);
  auto &SymSection = L.Sections[L.Symbols];
  auto &StrSection = L.Sections[L.Strings];
  const uint64_t OldCount = SymSection.sh_size / sizeof(typename ELF::Sym);
  std::vector<typename ELF::Sym> Symbols(OldCount);
  std::memcpy(Symbols.data(), Original.Raw.data() + SymSection.sh_offset,
              SymSection.sh_size);
  std::vector<uint8_t> Names(Original.Raw.begin() + StrSection.sh_offset,
                             Original.Raw.begin() + StrSection.sh_offset +
                                 StrSection.sh_size);
  if (Names.empty() || Names.front() || Names.back())
    return fail("invalid dynamic string-table boundary");
  auto NameAt = [&](uint64_t N) -> llvm::Expected<llvm::StringRef> {
    if (N >= Names.size())
      return fail("dynamic name is outside the string table");
    llvm::StringRef Tail(reinterpret_cast<const char *>(Names.data() + N),
                         Names.size() - N);
    const auto End = Tail.find('\0');
    if (End == llvm::StringRef::npos)
      return fail("unterminated dynamic name");
    return Tail.take_front(End);
  };
  auto AddName = [&](llvm::StringRef S) {
    const uint64_t N = Names.size();
    Names.insert(Names.end(), S.begin(), S.end());
    Names.push_back(0);
    return N;
  };
  std::set<std::string> Libraries;
  for (const auto &D : L.Entries)
    if (D.getTag() == DT_NEEDED) {
      auto Name = NameAt(D.getVal());
      if (!Name)
        return Name.takeError();
      Libraries.insert(Name->str());
    }
  for (const auto &Import : Imports) {
    if (Libraries.insert(Import.Request.Library).second) {
      typename ELF::Dyn D{};
      D.d_tag = DT_NEEDED;
      D.d_un.d_val = AddName(Import.Request.Library);
      L.Entries.push_back(D);
    }
    typename ELF::Sym S{};
    S.st_name = AddName(Import.Request.Symbol);
    S.setBindingAndType(STB_GLOBAL, Import.Request.Kind ==
                                            SourcePatchDependencyKind::Function
                                        ? STT_FUNC
                                        : STT_OBJECT);
    S.st_shndx = SHN_UNDEF;
    Symbols.push_back(S);
  }
  std::vector<uint8_t> Versions;
  if (L.Versions) {
    const auto &S = L.Sections[L.Versions];
    const auto *B = Original.readVA(S.sh_addr, S.sh_size);
    if (S.sh_addr != L.Tags[DT_VERSYM] || S.sh_size != OldCount * 2 || !B)
      return fail("version table does not cover exactly the old symbols");
    Versions.assign(B, B + S.sh_size);
    for (size_t I = 0; I < Imports.size(); ++I) {
      Versions.push_back(VER_NDX_GLOBAL);
      Versions.push_back(0);
    }
  }
  std::vector<uint8_t> Relocations;
  if (L.Tags[RelTag] || L.Tags[SizeTag]) {
    if (!L.Tags[RelTag] || !L.Tags[SizeTag] || L.Tags[EntTag] != sizeof(Rel) ||
        L.Tags[SizeTag] % sizeof(Rel) ||
        L.Tags[SizeTag] >
            ::neverd::limits::backend::kMaxELFDependencyRelocationBytes)
      return fail("invalid original dynamic relocation table");
    const auto *B = Original.readVA(L.Tags[RelTag], L.Tags[SizeTag]);
    if (!B)
      return fail("dynamic relocations are not file-backed");
    Relocations.assign(B, B + L.Tags[SizeTag]);
    if (L.Tags[DT_JMPREL] && L.Tags[DT_PLTRELSZ] &&
        (L.Tags[DT_JMPREL] > UINT64_MAX - L.Tags[DT_PLTRELSZ] ||
         L.Tags[RelTag] > UINT64_MAX - L.Tags[SizeTag] ||
         (L.Tags[DT_JMPREL] < L.Tags[RelTag] + L.Tags[SizeTag] &&
          L.Tags[RelTag] < L.Tags[DT_JMPREL] + L.Tags[DT_PLTRELSZ])))
      return fail("combined PLT/dynamic relocations require splitting before "
                  "extension");
    for (size_t I = 1; I < L.Sections.size(); ++I)
      if (L.Sections[I].sh_addr == L.Tags[RelTag] &&
          L.Sections[I].sh_type == (Rela ? SHT_RELA : SHT_REL)) {
        if (L.Relocations || L.Sections[I].sh_size != L.Tags[SizeTag] ||
            L.Sections[I].sh_link != L.Symbols)
          return fail("dynamic relocation section identity disagrees");
        L.Relocations = I;
      }
    if (!L.Relocations)
      return fail("dynamic relocations have no section identity");
  }
  uint32_t RelType;
  switch (Original.Arch) {
  case Arch::X86:
    RelType = R_386_GLOB_DAT;
    break;
  case Arch::X64:
    RelType = R_X86_64_GLOB_DAT;
    break;
  case Arch::ARM:
    RelType = R_ARM_GLOB_DAT;
    break;
  case Arch::AArch64:
    RelType = R_AARCH64_GLOB_DAT;
    break;
  default:
    return fail("unsupported dependency architecture");
  }
  ELFPatcher Patcher;
  Patcher.setInputKind(RewriteInputKind::ReplacementIR);
  const uint64_t Base =
      Patcher.plannedExecSegmentVA(Original.Raw, Original.Arch);
  if (!Base)
    return fail("cannot plan loader metadata storage");
  std::vector<uint8_t> Storage;
  auto Add = [&](const void *P, size_t Size, unsigned Alignment) {
    Storage.resize(llvm::alignTo(Storage.size(), Alignment), 0);
    const uint64_t Offset = Storage.size();
    Storage.resize(Offset + Size, 0);
    if (Size && P)
      std::memcpy(Storage.data() + Offset, P, Size);
    return Base + Offset;
  };
  const uint64_t OldRelocationSize = Relocations.size();
  for (size_t I = 0; I < Imports.size(); ++I) {
    Rel R{};
    R.setSymbolAndType(OldCount + I, RelType, false);
    if constexpr (Rela)
      R.r_addend = 0;
    const auto *B = reinterpret_cast<const uint8_t *>(&R);
    Relocations.insert(Relocations.end(), B, B + sizeof(R));
  }
  const uint64_t RelVA =
      Add(Relocations.data(), Relocations.size(), sizeof(typename ELF::Addr));
  const uint64_t SymVA =
      Add(Symbols.data(), Symbols.size() * sizeof(typename ELF::Sym),
          sizeof(typename ELF::Addr));
  const uint64_t StrVA = Add(Names.data(), Names.size(), 1);
  const uint64_t VerVA =
      Versions.empty() ? 0 : Add(Versions.data(), Versions.size(), 2);
  // A complete SysV table preserves all original symbol indices, including
  // versioned exports. GNU hash would require reordering its symbol suffix.
  const uint32_t Buckets = std::max<size_t>(1, Symbols.size() / 2);
  std::vector<uint32_t> Hash(2 + Buckets + Symbols.size(), 0);
  Hash[0] = Buckets;
  Hash[1] = Symbols.size();
  for (uint32_t I = 1; I < Symbols.size(); ++I) {
    auto Name = NameAt(Symbols[I].st_name);
    if (!Name)
      return Name.takeError();
    uint32_t H = 0;
    for (uint8_t C : *Name) {
      H = (H << 4) + C;
      const uint32_t G = H & 0xf0000000;
      H ^= G >> 24;
      H &= ~G;
    }
    uint32_t &Head = Hash[2 + H % Buckets];
    Hash[2 + Buckets + I] = Head;
    Head = I;
  }
  std::vector<uint8_t> HashBytes(Hash.size() * 4);
  for (size_t I = 0; I < Hash.size(); ++I)
    writeLE<uint32_t>(HashBytes.data() + I * 4, Hash[I]);
  const uint64_t HashVA = Add(HashBytes.data(), HashBytes.size(), 4);
  const std::map<int64_t, uint64_t> NewTags = {
      {DT_SYMTAB, SymVA},
      {DT_SYMENT, sizeof(typename ELF::Sym)},
      {DT_STRTAB, StrVA},
      {DT_STRSZ, Names.size()},
      {DT_HASH, HashVA},
      {RelTag, RelVA},
      {SizeTag, Relocations.size()},
      {EntTag, sizeof(Rel)}};
  std::set<int64_t> Written;
  std::vector<typename ELF::Dyn> Dynamic;
  for (auto D : L.Entries) {
    if (D.getTag() == DT_GNU_HASH)
      continue;
    if (auto It = NewTags.find(D.getTag()); It != NewTags.end()) {
      D.d_un.d_val = It->second;
      Written.insert(It->first);
    }
    if (D.getTag() == DT_VERSYM)
      D.d_un.d_val = VerVA;
    Dynamic.push_back(D);
  }
  for (const auto &[Tag, Value] : NewTags)
    if (!Written.count(Tag)) {
      typename ELF::Dyn D{};
      D.d_tag = Tag;
      D.d_un.d_val = Value;
      Dynamic.push_back(D);
    }
  Dynamic.emplace_back();
  // Read-only metadata does not inherit the slots' writable permissions.
  // PT_DYNAMIC stays writable because the loader may update DT_DEBUG.
  const uint64_t Data =
      llvm::alignTo(Base + Storage.size() + 1, elfPageSize(Original.Arch));
  Storage.resize(Data - Base, 0);
  const uint64_t Slots =
      Add(nullptr, Imports.size() * sizeof(typename ELF::Addr),
          sizeof(typename ELF::Addr));
  for (size_t I = 0; I < Imports.size(); ++I) {
    Imports[I].SlotVA = Slots + I * sizeof(typename ELF::Addr);
    Rel R;
    auto *Out =
        Storage.data() + RelVA - Base + OldRelocationSize + I * sizeof(Rel);
    std::memcpy(&R, Out, sizeof(R));
    R.r_offset = Imports[I].SlotVA;
    std::memcpy(Out, &R, sizeof(R));
  }
  const uint64_t DynVA =
      Add(Dynamic.data(), Dynamic.size() * sizeof(typename ELF::Dyn),
          sizeof(typename ELF::Addr));
  if (Storage.size() > ::neverd::limits::backend::
                           kELFSourceDependenciesAppendStorageSizeLimit ||
      (!L.Header.Is64 && Base + Storage.size() > UINT32_MAX))
    return fail("new loader metadata exceeds ELF address limits");
  auto Bytes = Original.Raw;
  if (Patcher.appendPatchSegments(Bytes, Storage, Original.Arch, Data,
                                  /*Executable=*/false) != Base)
    return fail("loader metadata placement changed");
  auto Offset = [&](uint64_t VA) -> uint64_t {
    uint64_t Result = UINT64_MAX;
    forEachELFPhdr(Bytes.data(), Bytes.size(),
                   [&](const ELFPhdrFields &P, const uint8_t *, bool) {
                     if (P.Type == PT_LOAD && VA >= P.VAddr &&
                         VA - P.VAddr < P.FileSz)
                       Result = P.Offset + VA - P.VAddr;
                   });
    return Result;
  };
  auto Move = [&](typename ELF::Shdr &S, uint64_t VA, uint64_t Size) {
    S.sh_addr = VA;
    S.sh_offset = Offset(VA);
    S.sh_size = Size;
  };
  Move(SymSection, SymVA, Symbols.size() * sizeof(typename ELF::Sym));
  Move(StrSection, StrVA, Names.size());
  if (L.Versions)
    Move(L.Sections[L.Versions], VerVA, Versions.size());
  if (L.Hash) {
    auto &S = L.Sections[L.Hash];
    Move(S, HashVA, HashBytes.size());
    S.sh_type = SHT_HASH;
    S.sh_entsize = 4;
    S.sh_addralign = 4;
    S.sh_link = L.Symbols;
  }
  if (!L.Relocations) {
    typename ELF::Shdr S{};
    S.sh_type = Rela ? SHT_RELA : SHT_REL;
    S.sh_flags = SHF_ALLOC;
    S.sh_link = L.Symbols;
    S.sh_addralign = sizeof(typename ELF::Addr);
    S.sh_entsize = sizeof(Rel);
    L.Relocations = L.Sections.size();
    L.Sections.push_back(S);
  }
  Move(L.Sections[L.Relocations], RelVA, Relocations.size());
  for (auto &S : L.Sections)
    if (S.sh_type == SHT_DYNAMIC && S.sh_addr == L.Dynamic.VAddr)
      Move(S, DynVA, Dynamic.size() * sizeof(typename ELF::Dyn));
  forEachELFPhdr(
      Bytes.data(), Bytes.size(),
      [&](ELFPhdrFields P, const uint8_t *Pointer, bool Is64) {
        if (P.Type != PT_DYNAMIC)
          return;
        P.VAddr = P.PAddr = DynVA;
        P.Offset = Offset(DynVA);
        P.FileSz = P.MemSz = Dynamic.size() * sizeof(typename ELF::Dyn);
        P.Flags = PF_R | PF_W;
        writeELFPhdr(Bytes.data() + (Pointer - Bytes.data()), Is64, P);
      });
  const uint64_t SectionOffset =
      llvm::alignTo(Bytes.size(), sizeof(typename ELF::Addr));
  if (L.Sections.size() >= SHN_LORESERVE ||
      SectionOffset + L.Sections.size() * sizeof(typename ELF::Shdr) >
          UINT32_MAX)
    return fail("new section table exceeds supported limits");
  Bytes.resize(SectionOffset + L.Sections.size() * sizeof(typename ELF::Shdr),
               0);
  std::memcpy(Bytes.data() + SectionOffset, L.Sections.data(),
              L.Sections.size() * sizeof(typename ELF::Shdr));
  auto *Header = reinterpret_cast<typename ELF::Ehdr *>(Bytes.data());
  Header->e_shoff = SectionOffset;
  Header->e_shnum = L.Sections.size();
  return Bytes;
}

template <class ELF>
llvm::Error verify(const BinaryImage &Image,
                   llvm::ArrayRef<SourcePatchImport> Imports) {
  using namespace llvm::ELF;
  constexpr bool Rela = sizeof(typename ELF::Addr) == 8;
  using Rel = std::conditional_t<Rela, typename ELF::Rela, typename ELF::Rel>;
  auto L = readLayout<ELF>(Image);
  if (!L)
    return L.takeError();
  const auto &Strings = L->Sections[L->Strings];
  llvm::StringRef Names(
      reinterpret_cast<const char *>(Image.Raw.data() + Strings.sh_offset),
      Strings.sh_size);
  std::set<std::string> Libraries;
  auto NameAt = [&](uint64_t Offset) -> llvm::Expected<llvm::StringRef> {
    if (Offset >= Names.size())
      return fail("dynamic name is outside its table");
    auto Tail = Names.drop_front(Offset);
    auto End = Tail.find('\0');
    if (End == llvm::StringRef::npos)
      return fail("unterminated dynamic name");
    return Tail.take_front(End);
  };
  for (const auto &D : L->Entries)
    if (D.getTag() == DT_NEEDED) {
      if (D.getVal() >= Names.size())
        return fail("invalid library-name offset");
      const auto Tail = Names.drop_front(D.getVal());
      const auto End = Tail.find('\0');
      if (End == llvm::StringRef::npos)
        return fail("unterminated library name");
      Libraries.insert(Tail.take_front(End).str());
    }
  const auto &SymSection = L->Sections[L->Symbols];
  const uint64_t SymbolCount = SymSection.sh_size / sizeof(typename ELF::Sym);
  const auto *Symbols = Image.readVA(L->Tags[DT_SYMTAB], SymSection.sh_size);
  const uint64_t RelVA = L->Tags[Rela ? DT_RELA : DT_REL];
  const uint64_t RelSize = L->Tags[Rela ? DT_RELASZ : DT_RELSZ];
  const uint64_t RelEntry = L->Tags[Rela ? DT_RELAENT : DT_RELENT];
  const auto *RelBytes = Image.readVA(RelVA, RelSize);
  if (!Symbols || !RelVA || !RelSize ||
      RelSize >
          ::neverd::limits::backend::kELFSourceDependenciesVerifyRelSizeLimit ||
      RelSize % sizeof(Rel) || RelEntry != sizeof(Rel) || !RelBytes)
    return fail("incomplete authoritative dynamic relocation table");
  // The OS follows PT_DYNAMIC, even when section headers describe some other
  // table. Authenticate both identities instead of trusting Image.Relocations.
  unsigned RelSections = 0;
  for (const auto &S : L->Sections)
    if (S.sh_type == (Rela ? SHT_RELA : SHT_REL) && S.sh_addr == RelVA) {
      if (S.sh_size != RelSize || S.sh_entsize != sizeof(Rel) ||
          S.sh_link != L->Symbols ||
          !rangeInBounds(S.sh_offset, RelSize, Image.Raw.size()) ||
          !std::equal(RelBytes, RelBytes + RelSize,
                      Image.Raw.begin() + S.sh_offset))
        return fail("dynamic relocation section differs from loader bytes");
      ++RelSections;
    }
  if (RelSections != 1)
    return fail("dynamic relocation table has no unique section identity");
  const auto *Hash = Image.readVA(L->Tags[DT_HASH], 8);
  if (!Hash || L->Tags[DT_GNU_HASH])
    return fail("new symbols require the rebuilt SysV hash table");
  const uint64_t Buckets = readLE<uint32_t>(Hash);
  if (!Buckets || Buckets > SymbolCount ||
      readLE<uint32_t>(Hash + 4) != SymbolCount)
    return fail("SysV hash does not cover the complete dynamic symbol table");
  Hash = Image.readVA(L->Tags[DT_HASH], (2 + Buckets + SymbolCount) * 4);
  if (!Hash)
    return fail("SysV hash is not file-backed");
  std::set<uint32_t> Hashed;
  for (uint32_t B = 0; B < Buckets; ++B) {
    for (uint32_t I = readLE<uint32_t>(Hash + (2 + B) * 4); I;
         I = readLE<uint32_t>(Hash + (2 + Buckets + I) * 4)) {
      if (I >= SymbolCount || !Hashed.insert(I).second)
        return fail("cyclic, duplicate or out-of-range SysV hash chain");
      typename ELF::Sym S;
      std::memcpy(&S, Symbols + I * sizeof(S), sizeof(S));
      auto Name = NameAt(S.st_name);
      if (!Name)
        return Name.takeError();
      uint32_t H = 0;
      for (uint8_t C : *Name) {
        H = (H << 4) + C;
        const uint32_t G = H & 0xf0000000;
        H ^= G >> 24;
        H &= ~G;
      }
      if (H % Buckets != B)
        return fail("SysV symbol is in the wrong hash bucket");
    }
  }
  if (Hashed.size() + 1 != SymbolCount)
    return fail("SysV hash omits a dynamic symbol");
  const uint8_t *Versions = nullptr;
  if (L->Tags[DT_VERSYM]) {
    Versions = Image.readVA(L->Tags[DT_VERSYM], SymbolCount * 2);
    if (!Versions || L->Sections[L->Versions].sh_size != SymbolCount * 2)
      return fail("symbol version table does not cover new symbols");
  }
  const uint32_t Type = Image.Arch == Arch::X86   ? R_386_GLOB_DAT
                        : Image.Arch == Arch::X64 ? R_X86_64_GLOB_DAT
                        : Image.Arch == Arch::ARM ? R_ARM_GLOB_DAT
                                                  : R_AARCH64_GLOB_DAT;
  for (const auto &Import : Imports) {
    const auto *Segment = Image.getSegmentFor(Import.SlotVA);
    unsigned Count = 0;
    for (uint64_t Offset = 0; Offset < RelSize; Offset += sizeof(Rel)) {
      Rel R;
      std::memcpy(&R, RelBytes + Offset, sizeof(R));
      if (R.r_offset == Import.SlotVA) {
        const uint32_t Index = R.getSymbol(false);
        if (Index >= SymbolCount || R.getType(false) != Type)
          return fail("added slot has a conflicting relocation");
        if constexpr (Rela)
          if (R.r_addend)
            return fail("added slot has a nonzero addend");
        typename ELF::Sym S;
        std::memcpy(&S, Symbols + Index * sizeof(S), sizeof(S));
        auto Name = NameAt(S.st_name);
        if (!Name)
          return Name.takeError();
        if (*Name != Import.Request.Symbol || S.st_shndx != SHN_UNDEF ||
            S.st_value || S.st_size || S.st_other ||
            S.getBinding() != STB_GLOBAL ||
            S.getType() !=
                (Import.Request.Kind == SourcePatchDependencyKind::Function
                     ? STT_FUNC
                     : STT_OBJECT) ||
            (Versions &&
             readLE<uint16_t>(Versions + Index * 2) != VER_NDX_GLOBAL))
          return fail(
              "added relocation does not name the requested global symbol");
        ++Count;
      }
    }
    if (Count != 1 || !Libraries.count(Import.Request.Library) || !Segment ||
        Segment->isExecutable() || !Segment->isWritable() ||
        !Segment->isReadable() ||
        !rangeInBounds(Import.SlotVA - Segment->VA, Image.getPointerSize(),
                       Segment->Size) ||
        !Image.readVA(Import.SlotVA, Image.getPointerSize()))
      return fail("added dependency failed dynamic-loader replay");
  }
  return llvm::Error::success();
}
} // namespace

llvm::Expected<std::vector<uint8_t>>
appendELFSourceDependencies(const BinaryImage &Original,
                            std::vector<SourcePatchImport> &Imports) {
  return Original.getPointerSize() == 8
             ? append<llvm::object::ELF64LE>(Original, Imports)
             : append<llvm::object::ELF32LE>(Original, Imports);
}
llvm::Error
verifyELFSourceDependencies(const BinaryImage &Image,
                            llvm::ArrayRef<SourcePatchImport> Imports) {
  return Image.getPointerSize() == 8
             ? verify<llvm::object::ELF64LE>(Image, Imports)
             : verify<llvm::object::ELF32LE>(Image, Imports);
}
} // namespace neverd
