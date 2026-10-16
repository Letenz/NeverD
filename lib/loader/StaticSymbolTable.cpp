//===- StaticSymbolTable.cpp - An image's static symbol table -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Each format's table is read the way its loader reads it, and only the
/// symbols a load would skip when the table is declined count: all of an ELF
/// image's .symtab, all of a PE's COFF symbol table, and a Mach-O image's
/// local and private external symbols.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/StaticSymbolTable.h"

#include "neverd/Common.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/ELF/ELFLoaderUtils.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/loader/ObjectFileUtils.h"
#include "neverd/support/FilePath.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"

namespace neverd {
namespace {

const char *tableName(BinaryFormat Format) {
  switch (Format) {
#define NEVERD_STATIC_SYMBOL_TABLE(Kind, Spelling)                             \
  case BinaryFormat::Kind:                                                     \
    return Spelling;
#include "neverd/loader/StaticSymbolTables.def"
  default:
    return "";
  }
}

/// The names one table holds, counted as a load reads them.  The sample
/// leads with global functions, the image's own code, before local ones,
/// which in a C program are mostly the toolchain's start-up files.
class Inventory {
public:
  Inventory(BinaryFormat Format, size_t MaxNames) : MaxNames(MaxNames) {
    Table.Table = tableName(Format);
  }

  void add(llvm::StringRef Name, bool IsFunction, bool IsGlobal) {
    ++Table.Count;
    auto &Sample = IsGlobal ? Global : Local;
    if (IsFunction && Sample.size() < MaxNames)
      Sample.push_back(Name.str());
  }

  std::optional<StaticSymbolTable> take() {
    if (!Table.Count)
      return std::nullopt;
    Table.Names = std::move(Global);
    for (auto &Name : Local)
      if (Table.Names.size() < MaxNames)
        Table.Names.push_back(std::move(Name));
    return std::move(Table);
  }

private:
  StaticSymbolTable Table;
  std::vector<std::string> Global, Local;
  size_t MaxNames;
};

template <typename ELFT>
std::optional<StaticSymbolTable>
elfTable(const llvm::object::ELFFile<ELFT> &ELF, size_t MaxNames) {
  using namespace llvm::ELF;
  // An object file's relocations name its symbols.
  if (ELF.getHeader().e_type == ET_REL)
    return std::nullopt;
  auto Sections = ELF.sections();
  if (!Sections) {
    llvm::consumeError(Sections.takeError());
    return std::nullopt;
  }
  Inventory Names(BinaryFormat::ELF, MaxNames);
  for (const auto &Header : *Sections) {
    if (Header.sh_type != SHT_SYMTAB)
      continue;
    auto Symbols = ELF.symbols(&Header);
    auto Strings = ELF.getStringTableForSymtab(Header, *Sections);
    if (!Symbols || !Strings) {
      if (!Symbols)
        llvm::consumeError(Symbols.takeError());
      if (!Strings)
        llvm::consumeError(Strings.takeError());
      continue;
    }
    for (const auto &Symbol : *Symbols) {
      // As collectSymbols reads them: defined, placed and named, without
      // section and file markers or the Arm toolchain's mapping symbols.
      const uint8_t Type = Symbol.getType();
      if (Symbol.st_shndx == SHN_UNDEF || Symbol.st_value == 0 ||
          Type == STT_SECTION || Type == STT_FILE)
        continue;
      auto Name = Symbol.getName(*Strings);
      if (!Name) {
        llvm::consumeError(Name.takeError());
        continue;
      }
      if (Name->empty() || elf_loader::isMappingSymbolName(*Name))
        continue;
      Names.add(*Name, Type == STT_FUNC || Type == STT_GNU_IFUNC,
                Symbol.getBinding() != STB_LOCAL);
    }
  }
  return Names.take();
}

std::optional<StaticSymbolTable>
coffTable(const llvm::object::COFFObjectFile &Obj, size_t MaxNames) {
  // An object file has no PE header, and its relocations name its symbols.
  if (!Obj.getPE32Header() && !Obj.getPE32PlusHeader())
    return std::nullopt;
  Inventory Names(BinaryFormat::COFF, MaxNames);
  for (const auto &Reference : Obj.symbols()) {
    const auto Symbol = Obj.getCOFFSymbol(Reference);
    auto Name = Obj.getSymbolName(Symbol);
    if (!Name) {
      llvm::consumeError(Name.takeError());
      continue;
    }
    // As parseSymbolTable reads them: named and defined, and placed at an
    // address, which a symbol outside every section has only as its value;
    // the names among them, without a section's own symbol.
    if (Name->empty() ||
        Symbol.getSectionNumber() == llvm::COFF::IMAGE_SYM_UNDEFINED ||
        (Symbol.getSectionNumber() < 0 && Symbol.getValue() == 0) ||
        coff_loader::isDebugRecord(Symbol) ||
        coff_loader::isSectionDefinition(Symbol))
      continue;
    Names.add(*Name,
              Symbol.getComplexType() == llvm::COFF::IMAGE_SYM_DTYPE_FUNCTION,
              Symbol.getStorageClass() == llvm::COFF::IMAGE_SYM_CLASS_EXTERNAL);
  }
  return Names.take();
}

std::optional<StaticSymbolTable>
machoTable(const llvm::object::MachOObjectFile &Obj, size_t MaxNames) {
  if (Obj.getHeader().filetype == llvm::MachO::MH_OBJECT)
    return std::nullopt;
  std::vector<bool> Instructions;
  for (const auto &Section : Obj.sections()) {
    const uint32_t Flags =
        Obj.is64Bit() ? Obj.getSection64(Section.getRawDataRefImpl()).flags
                      : Obj.getSection(Section.getRawDataRefImpl()).flags;
    Instructions.push_back((Flags & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
                                     llvm::MachO::S_ATTR_SOME_INSTRUCTIONS)) !=
                           0);
  }
  Inventory Names(BinaryFormat::MachO, MaxNames);
  for (const auto &Symbol : Obj.symbols()) {
    const auto Reference = Symbol.getRawDataRefImpl();
    uint8_t Type = 0, SectionIndex = 0;
    uint64_t Value = 0;
    if (Obj.is64Bit()) {
      const auto Entry = Obj.getSymbol64TableEntry(Reference);
      Type = Entry.n_type;
      SectionIndex = Entry.n_sect;
      Value = Entry.n_value;
    } else {
      const auto Entry = Obj.getSymbolTableEntry(Reference);
      Type = Entry.n_type;
      SectionIndex = Entry.n_sect;
      Value = Entry.n_value;
    }
    // As the loader reads them, and only those it skips when declined.
    const bool InSection =
        (Type & llvm::MachO::N_TYPE) == llvm::MachO::N_SECT &&
        SectionIndex > 0 && SectionIndex <= Instructions.size();
    if ((Type & llvm::MachO::N_STAB) != 0 ||
        (Type & llvm::MachO::N_TYPE) == llvm::MachO::N_UNDF ||
        (Value == 0 && !InSection) || !macho_loader::isStrippedSymbol(Type))
      continue;
    auto Name = Obj.getSymbolName(Reference);
    if (!Name) {
      llvm::consumeError(Name.takeError());
      continue;
    }
    if (Name->empty() ||
        ((Type & llvm::MachO::N_EXT) == 0 && Name->starts_with("ltmp")))
      continue;
    Names.add(*Name,
              InSection && Instructions[SectionIndex - 1] &&
                  !Name->starts_with(".L"),
              (Type & llvm::MachO::N_EXT) != 0);
  }
  return Names.take();
}

} // namespace

llvm::Expected<std::optional<StaticSymbolTable>>
findStaticSymbolTable(const std::filesystem::path &Path, size_t MaxNames) {
  auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(Path), /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  if (!Buffer)
    return llvm::createStringError(Buffer.getError(), "cannot read %s: %s",
                                   pathToUTF8(Path).c_str(),
                                   Buffer.getError().message().c_str());
  const auto Magic = llvm::identify_magic((*Buffer)->getBuffer());
  switch (magicToFormat(Magic)) {
  case BinaryFormat::MachO: {
    // The slice the loader reads from a universal file.
    auto Opened = macho_loader::openMachOFile(Path);
    if (!Opened) {
      llvm::consumeError(Opened.takeError());
      return std::nullopt;
    }
    return machoTable(*Opened->second, MaxNames);
  }
  case BinaryFormat::ELF:
  case BinaryFormat::COFF: {
    auto Object = llvm::object::ObjectFile::createObjectFile(
        (*Buffer)->getMemBufferRef());
    if (!Object) {
      llvm::consumeError(Object.takeError());
      return std::nullopt;
    }
    // The loaders read little-endian ELF only.
    if (const auto *ELF64 =
            llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(Object->get()))
      return elfTable(ELF64->getELFFile(), MaxNames);
    if (const auto *ELF32 =
            llvm::dyn_cast<llvm::object::ELF32LEObjectFile>(Object->get()))
      return elfTable(ELF32->getELFFile(), MaxNames);
    if (const auto *COFF =
            llvm::dyn_cast<llvm::object::COFFObjectFile>(Object->get()))
      return coffTable(*COFF, MaxNames);
    return std::nullopt;
  }
  default:
    return std::nullopt;
  }
}

} // namespace neverd
