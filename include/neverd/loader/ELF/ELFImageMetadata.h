//===- ELFImageMetadata.h - Loader-owned process image facts ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LOADER_ELF_ELFIMAGEMETADATA_H
#define NEVERD_LOADER_ELF_ELFIMAGEMETADATA_H

#include <cstdint>
#include <vector>

namespace neverd {
/// Decoded program-header fields in file order, including non-load records.
/// Consumers use these facts for OS policy without reparsing image bytes.
struct ELFProgramHeader {
  uint32_t Type, Flags;
  uint64_t FileOffset, VirtualAddress, FileSize, MemorySize, Alignment;
};

struct ELFImageMetadata {
  uint16_t Type;
  uint8_t OSABI, ABIVersion;
  uint32_t Flags;
  uint64_t ProgramHeaderFileOffset;
  uint16_t ProgramHeaderEntrySize;
  std::vector<ELFProgramHeader> ProgramHeaders;
  /// e_ehsize: the file header's own size.
  uint16_t HeaderSize = 0;
};
} // namespace neverd
#endif
