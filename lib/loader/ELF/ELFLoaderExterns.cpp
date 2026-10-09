//===- ELFLoaderExterns.cpp - Addresses of an object's externs ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A relocatable object names functions and data it does not define, and a
/// relocation against one of them has no address until a linker supplies
/// one.  The loader places each undefined symbol a relocation names in an
/// `extern` segment past every section, as IDA and Ghidra show an object's
/// externs, and names it there: a call to `exit` reaches the import `exit`
/// rather than address zero, where the object's first function sits, and a
/// read of `counter` reads the symbol `counter`.  Another
/// module defines what the segment holds, so it is writable and states
/// nothing.  A common symbol gets its storage there too, as a linker
/// allocates it in .bss.  A GOT reference reaches its symbol through an entry
/// the linker would create; the loader makes those entries in a read-only
/// `.got` past the externs, each holding its symbol's address.
///
//===----------------------------------------------------------------------===//

#include "ELFLoaderDetail.h"

#include "neverd/Limits.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/object/SectionNames.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <string>

namespace neverd {
namespace elf_loader {
namespace detail {

std::optional<uint32_t> directTypeOfGOTReference(Arch A, uint32_t Type) {
  using namespace llvm::ELF;
#define NEVERD_ELF_GOT_REFERENCE(TheArch, GotType, DirectType)                 \
  if (A == Arch::TheArch && Type == GotType)                                   \
    return DirectType;
#include "ELFObjectRelocations.def"
  return std::nullopt;
}

bool isBranchReference(Arch A, uint32_t Type) {
  using namespace llvm::ELF;
#define NEVERD_ELF_BRANCH_REFERENCE(TheArch, BranchType)                       \
  if (A == Arch::TheArch && Type == BranchType)                                \
    return true;
#include "ELFObjectRelocations.def"
  return false;
}

namespace {

/// The `_GLOBAL_OFFSET_TABLE_` an i386 or ARM object addresses its GOT from
/// is the linker's, not a function or datum of another module.
constexpr llvm::StringLiteral kGlobalOffsetTable("_GLOBAL_OFFSET_TABLE_");

/// Alignment of the extern segment, of the GOT, and of each data extern.
constexpr uint64_t kExternAlignment = 16;

/// One relocation of an allocated section, as the extern plan reads it.
struct ObjectRelocation {
  uint32_t SymbolTable = 0;
  uint32_t SymbolIndex = 0;
  uint32_t Type = 0;
  /// A REL relocation's addend is in the relocated field; it reads as zero.
  int64_t Addend = 0;
  bool InCode = false;
};

/// Calls \p Visit(Relocation, Symbol, Name) for every relocation of
/// \p Sections that applies to an allocated section and names a symbol.
template <typename ELFT, typename Fn>
void forEachObjectRelocation(const llvm::object::ELFFile<ELFT> &ELF,
                             llvm::ArrayRef<typename ELFT::Shdr> Sections,
                             const uint8_t *Data, size_t Size, Fn Visit) {
  using namespace llvm::ELF;
  using Elf_Rel = typename ELFT::Rel;
  using Elf_Rela = typename ELFT::Rela;
  for (const auto &SH : Sections) {
    const bool IsRela = SH.sh_type == SHT_RELA;
    if (!IsRela && SH.sh_type != SHT_REL)
      continue;
    const size_t EntrySize = IsRela ? sizeof(Elf_Rela) : sizeof(Elf_Rel);
    if (SH.sh_entsize < EntrySize ||
        !rangeInBounds(SH.sh_offset, SH.sh_size, Size))
      continue;
    const auto *ApplySH = getShdr<ELFT>(Sections, SH.sh_info);
    const auto *SymSH = getShdr<ELFT>(Sections, SH.sh_link);
    if (!ApplySH || !(ApplySH->sh_flags & SHF_ALLOC) || !SymSH)
      continue;
    auto SymsOr = ELF.symbols(SymSH);
    auto StrTabOr = ELF.getStringTableForSymtab(*SymSH);
    if (!SymsOr || !StrTabOr) {
      if (!SymsOr)
        llvm::consumeError(SymsOr.takeError());
      if (!StrTabOr)
        llvm::consumeError(StrTabOr.takeError());
      continue;
    }
    ObjectRelocation Rel;
    Rel.SymbolTable = SH.sh_link;
    Rel.InCode = (ApplySH->sh_flags & SHF_EXECINSTR) != 0;
    const size_t Count = static_cast<size_t>(SH.sh_size / SH.sh_entsize);
    for (size_t I = 0; I < Count; ++I) {
      const uint64_t Off =
          SH.sh_offset + static_cast<uint64_t>(I) * SH.sh_entsize;
      if (!rangeInBounds(Off, EntrySize, Size))
        break;
      if (IsRela) {
        Elf_Rela R;
        std::memcpy(&R, Data + static_cast<size_t>(Off), sizeof(R));
        Rel.Type = R.getType(false);
        Rel.SymbolIndex = R.getSymbol(false);
        Rel.Addend = R.r_addend;
      } else {
        Elf_Rel R;
        std::memcpy(&R, Data + static_cast<size_t>(Off), sizeof(R));
        Rel.Type = R.getType(false);
        Rel.SymbolIndex = R.getSymbol(false);
        Rel.Addend = 0;
      }
      if (Rel.SymbolIndex == 0 || Rel.SymbolIndex >= SymsOr->size())
        continue;
      const auto &Sym = (*SymsOr)[Rel.SymbolIndex];
      llvm::StringRef Name;
      if (auto NameOr = Sym.getName(*StrTabOr))
        Name = *NameOr;
      else
        llvm::consumeError(NameOr.takeError());
      Visit(Rel, Sym, Name);
    }
  }
}

llvm::Error noExternRoom() {
  return llvm::make_error<llvm::StringError>(
      "elf: no address room past the object's sections for its externs",
      llvm::inconvertibleErrorCode());
}

/// Advance \p Next past one item of \p Bytes aligned to \p Alignment and
/// return the item's address, or nullopt when the address space ends first.
std::optional<va_t> allocate(va_t &Next, uint64_t Bytes, uint64_t Alignment) {
  if (Next > InvalidVA - (Alignment - 1))
    return std::nullopt;
  const va_t At = llvm::alignTo(Next, Alignment);
  if (Bytes > InvalidVA - At)
    return std::nullopt;
  Next = At + Bytes;
  return At;
}

} // namespace

template <typename ELFT>
llvm::Expected<ObjectExterns>
planObjectExterns(const llvm::object::ELFFile<ELFT> &ELF,
                  llvm::ArrayRef<typename ELFT::Shdr> Sections,
                  const uint8_t *Data, size_t Size, Arch A, va_t ImageEnd) {
  using namespace llvm::ELF;
  struct Undefined {
    std::string Name;
    bool Called = false;
    uint64_t StatedReach = 0;
  };
  struct Common {
    std::string Name;
    uint64_t Bytes = 0;
    uint64_t Alignment = 1;
  };
  std::vector<Undefined> Undefineds;
  std::map<std::string, size_t> UndefinedIndex;
  std::vector<Common> Commons;
  std::set<std::string> SeenCommons;
  std::vector<std::pair<uint32_t, uint32_t>> Referenced;
  std::set<std::pair<uint32_t, uint32_t>> SeenReferenced;
  forEachObjectRelocation<ELFT>(
      ELF, Sections, Data, Size,
      [&](const ObjectRelocation &Rel, const typename ELFT::Sym &Sym,
          llvm::StringRef Name) {
        if (directTypeOfGOTReference(A, Rel.Type) &&
            SeenReferenced.insert({Rel.SymbolTable, Rel.SymbolIndex}).second)
          Referenced.push_back({Rel.SymbolTable, Rel.SymbolIndex});
        if (Name.empty())
          return;
        if (Sym.st_shndx == SHN_COMMON) {
          const uint64_t Alignment = Sym.st_value;
          if (SeenCommons.insert(Name.str()).second)
            Commons.push_back({Name.str(), Sym.st_size,
                               llvm::isPowerOf2_64(Alignment) ? Alignment : 1});
          return;
        }
        if (Sym.st_shndx != SHN_UNDEF || Name == kGlobalOffsetTable)
          return;
        auto [It, Inserted] =
            UndefinedIndex.try_emplace(Name.str(), Undefineds.size());
        if (Inserted)
          Undefineds.push_back({Name.str()});
        Undefined &U = Undefineds[It->second];
        U.Called |= Rel.InCode && isBranchReference(A, Rel.Type);
        if (Rel.Addend > 0)
          U.StatedReach =
              std::max(U.StatedReach, static_cast<uint64_t>(Rel.Addend));
      });
  ObjectExterns Externs;
  if (Undefineds.empty() && Commons.empty() && Referenced.empty())
    return Externs;

  const uint64_t SlotBytes = sizeof(typename ELFT::Addr);
  va_t Next = ImageEnd;
  auto Base = allocate(Next, 0, kExternAlignment);
  if (!Base)
    return noExternRoom();
  Externs.ExternBase = *Base;
  for (const Undefined &U : Undefineds) {
    // A function is called, never read at an offset.  Data keeps room past
    // its address, so a field another module defines is not another extern.
    uint64_t Bytes = SlotBytes, Alignment = SlotBytes;
    if (!U.Called) {
      Bytes = std::min(U.StatedReach, limits::kMaxObjectExternStatedReach) +
              limits::kObjectExternDataReach;
      Alignment = kExternAlignment;
    }
    auto At = allocate(Next, Bytes, Alignment);
    if (!At)
      return noExternRoom();
    Externs.SymbolSlots[U.Name] = *At;
    if (U.Called)
      Externs.CalledSymbols.insert(U.Name);
  }
  for (const Common &C : Commons) {
    auto At = allocate(Next, std::max<uint64_t>(C.Bytes, 1),
                       std::max(C.Alignment, kExternAlignment));
    if (!At)
      return noExternRoom();
    Externs.CommonSlots[C.Name] = *At;
  }
  Externs.ExternSize = Next - Externs.ExternBase;

  if (!Referenced.empty()) {
    auto GOT = allocate(Next, 0, kExternAlignment);
    if (!GOT || Referenced.size() > (InvalidVA - *GOT) / SlotBytes)
      return noExternRoom();
    Externs.GOTBase = *GOT;
    Externs.GOTSize = Referenced.size() * SlotBytes;
    for (size_t I = 0; I < Referenced.size(); ++I)
      Externs.GOTEntries[Referenced[I]] = *GOT + I * SlotBytes;
  }
  return Externs;
}

template <typename ELFT>
void addObjectExterns(const llvm::object::ELFFile<ELFT> &ELF,
                      llvm::ArrayRef<typename ELFT::Shdr> Sections,
                      const std::vector<va_t> &SecBase,
                      const ObjectExterns &Externs, BinaryImage &Img) {
  using namespace llvm::ELF;
  constexpr size_t SlotBytes = sizeof(typename ELFT::Addr);
  auto AddRegion = [&](llvm::StringRef Name, va_t VA, uint64_t Bytes,
                       SegmentFlags Flags, std::vector<uint8_t> Contents) {
    Section Sec;
    Sec.Name = Name.str();
    Sec.VA = VA;
    Sec.Size = Bytes;
    Sec.Flags = Flags;
    Sec.Alignment = kExternAlignment;
    Segment Seg;
    Seg.Name = Name.str();
    Seg.VA = VA;
    Seg.Size = Bytes;
    Seg.Flags = Flags;
    if (hasFlag(Flags, SegmentFlags::Writable)) {
      // Zero-filled like an object's .bss: the object states nothing here.
      Sec.Type = SHT_NOBITS;
      Seg.Data.assign(static_cast<size_t>(Bytes), 0);
    } else {
      Sec.Type = SHT_PROGBITS;
      Sec.FileSz = Bytes;
      Sec.Data = Contents;
      Seg.FileSz = Bytes;
      Seg.Data = std::move(Contents);
      Seg.ReadOnlyAfterRelocations = true;
    }
    Img.Sections.push_back(std::move(Sec));
    Img.Segments.push_back(std::move(Seg));
  };

  if (Externs.ExternSize != 0)
    AddRegion(section_names::elf::SynthesizedExtern, Externs.ExternBase,
              Externs.ExternSize,
              SegmentFlags::Readable | SegmentFlags::Writable, {});
  for (const auto &[Name, SlotVA] : Externs.SymbolSlots) {
    // A called extern's address is the import's callable identity, as a
    // Mach-O stub's is.  Data is a variable another module defines: a symbol,
    // and no import, whose address would read as an import slot.
    const bool Called = Externs.CalledSymbols.count(Name) != 0;
    if (Called) {
      Import Imp;
      Imp.Module = kExternModule.str();
      Imp.Name = Name;
      Imp.IATAddr = SlotVA;
      Img.Imports.push_back(std::move(Imp));
    }
    // Another module states the size of what it defines; the object does
    // not, so the symbol has none.
    Symbol Sym;
    Sym.Name = Name;
    Sym.Addr = SlotVA;
    Sym.IsFunc = Called;
    Img.Symbols.push_back(std::move(Sym));
  }

  if (Externs.GOTSize == 0)
    return;
  // A GOT entry holds its symbol's address: the extern address of an
  // undefined or common symbol, where the object places a defined one.
  std::vector<uint8_t> GOT(static_cast<size_t>(Externs.GOTSize), 0);
  std::vector<std::pair<va_t, std::string>> ImportEntries;
  std::vector<std::pair<va_t, std::pair<va_t, va_t>>> PointerEntries;
  for (const auto &[Key, EntryVA] : Externs.GOTEntries) {
    const auto *SymSH = getShdr<ELFT>(Sections, Key.first);
    if (!SymSH)
      continue;
    auto SymsOr = ELF.symbols(SymSH);
    auto StrTabOr = ELF.getStringTableForSymtab(*SymSH);
    if (!SymsOr || !StrTabOr || Key.second >= SymsOr->size()) {
      if (!SymsOr)
        llvm::consumeError(SymsOr.takeError());
      if (!StrTabOr)
        llvm::consumeError(StrTabOr.takeError());
      continue;
    }
    const auto &Sym = (*SymsOr)[Key.second];
    std::string Name;
    if (auto NameOr = Sym.getName(*StrTabOr))
      Name = NameOr->str();
    else
      llvm::consumeError(NameOr.takeError());
    std::optional<va_t> Address;
    va_t OwnerVA = InvalidVA;
    if (Sym.st_shndx == SHN_UNDEF) {
      // The entry of a called extern holds the import, so a call through it
      // reaches the import; data's holds the address of its storage here.
      if (auto It = Externs.SymbolSlots.find(Name);
          It != Externs.SymbolSlots.end()) {
        Address = It->second;
        if (Externs.CalledSymbols.count(Name))
          ImportEntries.push_back({EntryVA, Name});
        else
          OwnerVA = Externs.ExternBase;
      }
    } else if (Sym.st_shndx == SHN_COMMON) {
      if (auto It = Externs.CommonSlots.find(Name);
          It != Externs.CommonSlots.end()) {
        Address = It->second;
        OwnerVA = Externs.ExternBase;
      }
    } else if (Sym.st_shndx == SHN_ABS) {
      Address = static_cast<va_t>(Sym.st_value);
    } else if (Sym.st_shndx < SHN_LORESERVE && Sym.st_shndx < SecBase.size() &&
               static_cast<va_t>(Sym.st_value) <=
                   InvalidVA - SecBase[Sym.st_shndx]) {
      Address = SecBase[Sym.st_shndx] + static_cast<va_t>(Sym.st_value);
      OwnerVA = SecBase[Sym.st_shndx];
    }
    if (!Address)
      continue;
    const uint64_t Value = *Address;
    std::memcpy(GOT.data() + (EntryVA - Externs.GOTBase), &Value, SlotBytes);
    if (OwnerVA != InvalidVA)
      PointerEntries.push_back({EntryVA, {*Address, OwnerVA}});
  }
  AddRegion(section_names::elf::Got, Externs.GOTBase, Externs.GOTSize,
            SegmentFlags::Readable, std::move(GOT));
  for (const auto &[EntryVA, Name] : ImportEntries)
    Img.recordImportStorageSlot(EntryVA, Name, 0,
                                ImportStorageEvidence::LoaderBind);
  for (const auto &[EntryVA, Target] : PointerEntries)
    recordAbsolutePointerRelocation(Img, EntryVA, Target.first, Target.second);
}

template llvm::Expected<ObjectExterns> planObjectExterns<llvm::object::ELF32LE>(
    const llvm::object::ELFFile<llvm::object::ELF32LE> &,
    llvm::ArrayRef<llvm::object::ELF32LE::Shdr>, const uint8_t *, size_t, Arch,
    va_t);
template llvm::Expected<ObjectExterns> planObjectExterns<llvm::object::ELF64LE>(
    const llvm::object::ELFFile<llvm::object::ELF64LE> &,
    llvm::ArrayRef<llvm::object::ELF64LE::Shdr>, const uint8_t *, size_t, Arch,
    va_t);
template void addObjectExterns<llvm::object::ELF32LE>(
    const llvm::object::ELFFile<llvm::object::ELF32LE> &,
    llvm::ArrayRef<llvm::object::ELF32LE::Shdr>, const std::vector<va_t> &,
    const ObjectExterns &, BinaryImage &);
template void addObjectExterns<llvm::object::ELF64LE>(
    const llvm::object::ELFFile<llvm::object::ELF64LE> &,
    llvm::ArrayRef<llvm::object::ELF64LE::Shdr>, const std::vector<va_t> &,
    const ObjectExterns &, BinaryImage &);

} // namespace detail
} // namespace elf_loader
} // namespace neverd
