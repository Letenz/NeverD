//===- COFFObjectExterns.cpp - Addresses of a COFF object's externs -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Collects what a COFF object references without defining: the undefined
/// symbols its relocations name, whether a call or branch reaches each, its
/// common symbols (an undefined external whose value is its size), and its
/// `__imp_` symbols, each a cell holding its function's extern address.
///
//===----------------------------------------------------------------------===//

#include "COFFObjectExterns.h"

#include "neverd/loader/SymbolDecoration.h"
#include "neverd/object/SectionNames.h"

#include "llvm/BinaryFormat/COFF.h"

#include <set>

namespace neverd {
namespace coff_loader {

bool isBranchReference(uint16_t Machine, uint32_t Type,
                       llvm::ArrayRef<uint8_t> Contents, uint64_t Offset,
                       bool InCode) {
  using namespace llvm::COFF;
  if (!InCode)
    return false;
#define NEVERD_COFF_BRANCH_REFERENCE(TheMachine, BranchType)                   \
  if (Machine == TheMachine && Type == BranchType)                             \
    return true;
#define NEVERD_COFF_BRANCH_BY_OPCODE(TheMachine, BranchType, Opcode)           \
  if (Machine == TheMachine && Type == BranchType && Offset > 0 &&             \
      Offset <= Contents.size() && Contents[Offset - 1] == Opcode)             \
    return true;
#include "COFFObjectRelocations.def"
  return false;
}

llvm::Expected<ObjectExterns>
planObjectExterns(const llvm::object::COFFObjectFile &Obj,
                  const std::vector<va_t> &SectionVAs, va_t ImageEnd) {
  using namespace llvm::COFF;
  const uint16_t Machine = Obj.getMachine();
  const llvm::StringRef CellPrefix = importSlotPrefix(BinaryFormat::COFF);
  object_externs::ExternRequests Requests;
  std::vector<std::string> Cells;
  std::set<std::string> SeenCells;
  // An undefined external whose value is not zero is a common symbol of that
  // many bytes, which a linker allocates whether a relocation names it or not.
  auto NoteCommon = [&](llvm::object::COFFSymbolRef Sym, llvm::StringRef Name) {
    if (Sym.getSectionNumber() == IMAGE_SYM_UNDEFINED && Sym.getValue() != 0 &&
        Sym.isExternal())
      Requests.noteCommon(Name, Sym.getValue(), 1);
  };
  for (const llvm::object::SectionRef &SecRef : Obj.sections()) {
    const unsigned SectionID = Obj.getSectionID(SecRef);
    if (SectionID >= SectionVAs.size() || SectionVAs[SectionID] == InvalidVA)
      continue;
    const llvm::object::coff_section *Sec = Obj.getCOFFSection(SecRef);
    llvm::ArrayRef<uint8_t> Contents;
    if (llvm::Error Err = Obj.getSectionContents(Sec, Contents))
      llvm::consumeError(std::move(Err));
    const bool InCode = (Sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    for (const llvm::object::RelocationRef &Reloc : SecRef.relocations()) {
      auto SymIt = Reloc.getSymbol();
      if (SymIt == Obj.symbol_end())
        continue;
      // A weak external's address is the null test the code makes of it,
      // which any address the loader gave it would decide; its default
      // stays the linker's.
      const llvm::object::COFFSymbolRef Sym = Obj.getCOFFSymbol(*SymIt);
      if (Sym.getSectionNumber() != IMAGE_SYM_UNDEFINED || Sym.isWeakExternal())
        continue;
      auto NameOr = SymIt->getName();
      if (!NameOr) {
        llvm::consumeError(NameOr.takeError());
        continue;
      }
      const llvm::StringRef Name = *NameOr;
      if (Sym.getValue() != 0) {
        NoteCommon(Sym, Name);
        continue;
      }
      // An import library supplies the pointer `__imp_f` to its function f.
      if (Name.starts_with(CellPrefix) && Name.size() > CellPrefix.size()) {
        if (SeenCells.insert(Name.str()).second)
          Cells.push_back(Name.str());
        Requests.noteUndefined(Name.drop_front(CellPrefix.size()),
                               /*Called=*/true, 0);
        continue;
      }
      Requests.noteUndefined(Name,
                             isBranchReference(Machine, Reloc.getType(),
                                               Contents, Reloc.getOffset(),
                                               InCode),
                             0);
    }
  }
  for (const llvm::object::SymbolRef &SymRef : Obj.symbols()) {
    auto NameOr = SymRef.getName();
    if (!NameOr) {
      llvm::consumeError(NameOr.takeError());
      continue;
    }
    NoteCommon(Obj.getCOFFSymbol(SymRef), *NameOr);
  }
  auto LayoutOr = object_externs::layoutExterns(
      Requests, Cells.size(), Obj.getBytesInAddress(), ImageEnd);
  if (!LayoutOr)
    return LayoutOr.takeError();
  ObjectExterns Externs;
  static_cast<object_externs::ExternLayout &>(Externs) = std::move(*LayoutOr);
  for (size_t I = 0; I < Cells.size(); ++I)
    Externs.ImportCells[Cells[I]] = Externs.cellAddress(I);
  return Externs;
}

void addObjectExterns(const ObjectExterns &Externs, Arch Target,
                      BinaryImage &Img) {
  auto ImportName = [Target](llvm::StringRef Symbol) {
    return importNameOfSymbol(Symbol, BinaryFormat::COFF, Target).str();
  };
  object_externs::addExternSegment(Externs, Img, ImportName);
  // The symbol table names no undefined symbol, so the common ones are
  // published here, at their storage.
  for (const auto &[Name, StorageVA] : Externs.CommonSlots) {
    Symbol Sym;
    Sym.Name = Name;
    Sym.Addr = StorageVA;
    if (auto It = Externs.CommonSizes.find(Name);
        It != Externs.CommonSizes.end())
      Sym.Size = It->second;
    Img.Symbols.push_back(std::move(Sym));
  }
  if (Externs.ImportCells.empty())
    return;
  const llvm::StringRef CellPrefix = importSlotPrefix(BinaryFormat::COFF);
  std::vector<uint8_t> Contents;
  std::vector<std::pair<va_t, std::string>> Slots;
  for (const auto &[CellSymbol, CellVA] : Externs.ImportCells) {
    const llvm::StringRef Function =
        llvm::StringRef(CellSymbol).drop_front(CellPrefix.size());
    const auto Slot = Externs.SymbolSlots.find(Function.str());
    if (Slot == Externs.SymbolSlots.end())
      continue;
    object_externs::writeCell(
        Externs, Contents,
        static_cast<size_t>((CellVA - Externs.CellBase) / Externs.PointerBytes),
        Slot->second);
    Slots.push_back({CellVA, ImportName(Function)});
  }
  object_externs::addCellSegment(Externs, section_names::coff::Idata,
                                 std::move(Contents), Img);
  for (const auto &[CellVA, Name] : Slots)
    Img.recordImportStorageSlot(CellVA, Name, 0,
                                ImportStorageEvidence::LoaderBind);
}

std::optional<std::pair<va_t, va_t>>
resolveObjectExtern(const ObjectExterns &Externs, llvm::StringRef Name,
                    bool Common, bool Branch) {
  if (Common) {
    if (auto It = Externs.CommonSlots.find(Name.str());
        It != Externs.CommonSlots.end())
      return std::make_pair(It->second, Externs.ExternBase);
    return std::nullopt;
  }
  if (auto It = Externs.ImportCells.find(Name.str());
      It != Externs.ImportCells.end())
    return std::make_pair(It->second, Externs.CellBase);
  if (auto It = Externs.SymbolSlots.find(Name.str());
      It != Externs.SymbolSlots.end())
    return std::make_pair(It->second, Branch ? InvalidVA : Externs.ExternBase);
  return std::nullopt;
}

} // namespace coff_loader
} // namespace neverd
