//===- ELFProgramMetadata.h - Program-owned dynamic table facts -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LOADER_ELF_ELFPROGRAMMETADATA_H
#define NEVERD_LOADER_ELF_ELFPROGRAMMETADATA_H

#include "llvm/Support/Error.h"

#include <cstdint>
#include <vector>

namespace neverd {
struct BinaryImage;
struct ELFDynamicEntry {
  int64_t Tag;
  uint64_t Value;
};
/// Decode the original PT_DYNAMIC file table, independently of sections and
/// analysis fixups. Reject duplicate, truncated or unterminated tables. An
/// absent table returns an empty vector; entries exclude the DT_NULL sentinel.
/// OS/linker policy owns the meaning of the resulting tags and addresses.
llvm::Expected<std::vector<ELFDynamicEntry>>
readELFProgramDynamicTable(const BinaryImage &Image);
} // namespace neverd
#endif
