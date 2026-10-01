//===- ELFProgramLinking.h - Original dynamic linking records ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LOADER_ELF_ELFPROGRAMLINKING_H
#define NEVERD_LOADER_ELF_ELFPROGRAMLINKING_H

#include "neverd/loader/ELF/ELFProgramMetadata.h"

#include <string>

namespace neverd {
struct ELFProgramSymbol {
  std::string Name;
  uint64_t Value, Size;
  uint16_t SectionIndex;
  uint8_t Info, Other;
};
struct ELFProgramRelocation {
  uint64_t Address;
  int64_t Addend;
  uint32_t Type, Symbol;
  bool ExplicitAddend;
};
struct ELFProgramLinking {
  std::vector<ELFDynamicEntry> Dynamic;
  std::vector<ELFProgramSymbol> Symbols;
  std::vector<ELFProgramRelocation> Relocations;
  std::vector<std::string> Needed;
};
/// ELF64LE linking facts from original PT_LOAD/PT_DYNAMIC bytes, independent
/// of section headers and analysis relocations. RELA, Android APS2 RELA and
/// RELR are decoded; interpreting relocation types remains the linker's job.
/// Reject malformed, ambiguous, unsupported and excessive metadata.
llvm::Expected<ELFProgramLinking>
readELFProgramLinking(const BinaryImage &Image);
} // namespace neverd
#endif
