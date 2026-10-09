//===- DataSymbolBinding.cpp - Symbol-bound data pointer storage ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/loader/DataSymbolBinding.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ELF/ELFLoaderUtils.h"

#include "llvm/BinaryFormat/ELF.h"

#include <stdexcept>

namespace neverd {
namespace {
bool isDataSlot(Arch Target, uint32_t Type) {
#define NEVERD_ELF_DATA_SLOT(ISA, Reloc)                                       \
  if (Target == Arch::ISA && Type == llvm::ELF::Reloc)                         \
    return true;
#include "ELF/ELFDynamicRelocations.def"
  return false;
}

bool immutableSlot(const BinaryImage &Image, va_t Slot) {
  const auto *Segment = Image.getSegmentFor(Slot);
  const unsigned Width = Image.getPointerSize();
  if (!Segment || !Image.readVA(Slot, Width))
    return false;
  if (!Segment->isWritable() || Segment->ReadOnlyAfterRelocations)
    return true;
  if (Image.ELFMetadata)
    for (const auto &Header : Image.ELFMetadata->ProgramHeaders)
      if (Header.Type == llvm::ELF::PT_GNU_RELRO &&
          Slot >= Header.VirtualAddress &&
          Slot - Header.VirtualAddress <= Header.MemorySize &&
          Width <= Header.MemorySize - (Slot - Header.VirtualAddress))
        return true;
  return false;
}
} // namespace

std::map<va_t, DataSymbolBinding>
collectDataSymbolBindings(const BinaryImage &Image) {
  std::map<va_t, DataSymbolBinding> Bindings;
  if (!Image.isELF() || Image.IsRelocatable)
    return Bindings;
  for (const auto &Relocation : Image.Relocations) {
    if (!isDataSlot(Image.Arch, Relocation.Type) || !Relocation.ELF ||
        !Relocation.ELF->Symbol)
      continue;
    const auto &Symbol = *Relocation.ELF->Symbol;
    if (!Symbol.Name || Symbol.Name->empty() ||
        (Symbol.Type != llvm::ELF::STT_OBJECT &&
         Symbol.Type != llvm::ELF::STT_NOTYPE) ||
        !Image.readVA(Relocation.Address, Image.getPointerSize()))
      continue;
    DataSymbolBinding Binding;
    Binding.Name = *Symbol.Name;
    Binding.Weak = Symbol.Binding == llvm::ELF::STB_WEAK;
    Binding.Immutable = immutableSlot(Image, Relocation.Address);
    if (Symbol.isDefined()) {
      if (Symbol.SectionIndex >= llvm::ELF::SHN_LORESERVE ||
          !Image.readVA(Symbol.Value, 1) || Image.isCodeAddress(Symbol.Value))
        continue;
      Binding.Definition = Symbol.Value;
    }
    // GLOB_DAT ignores the encoded slot for REL on x86 and ARM. AArch64
    // uses an explicit RELA addend; never borrow already relocated bytes.
    const auto UsesAddend =
        elf_loader::symbolRelocationValue(Image.Arch, Relocation.Type, 0, 1);
    if (!UsesAddend)
      throw std::invalid_argument("data relocation has no symbol-value policy");
    if (*UsesAddend != 0) {
      if (!Relocation.HasExplicitAddend)
        continue;
      Binding.Addend = Relocation.Addend;
    }
    auto [It, Inserted] = Bindings.emplace(Relocation.Address, Binding);
    if (!Inserted && It->second != Binding)
      throw std::invalid_argument(
          "conflicting data symbol relocation bindings");
  }
  return Bindings;
}
} // namespace neverd
