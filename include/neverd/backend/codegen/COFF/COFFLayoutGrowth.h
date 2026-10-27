//===- COFFLayoutGrowth.h - PE header storage growth -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Grow PE section-header storage without changing existing section RVAs.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_COFF_COFFLAYOUTGROWTH_H
#define NEVERD_BACKEND_CODEGEN_COFF_COFFLAYOUTGROWTH_H

#include "llvm/Support/Error.h"

#include <cstdint>
#include <vector>

namespace neverd {

struct COFFHeaderGrowth {
  uint32_t OldSizeOfHeaders = 0;
  uint32_t NewSizeOfHeaders = 0;
  uint32_t OldPEOffset = 0;
  uint32_t NewPEOffset = 0;

  uint32_t addedFileBytes() const {
    return NewSizeOfHeaders - OldSizeOfHeaders;
  }
};

/// Reserve AdditionalSections consecutive headers after the existing table.
/// All existing RVAs remain unchanged. File-backed sections and overlay data
/// move together; every modeled PE file pointer is adjusted. Relocate the NT
/// headers into newly allocated storage, retaining old header bytes even when
/// nested metadata references them. No bytes change on error.
/// Growth that would overlap the first section's RVA is not representable by
/// this operation and is rejected.
llvm::Expected<COFFHeaderGrowth>
ensureCOFFSectionHeaderCapacity(std::vector<uint8_t> &Binary,
                                uint16_t AdditionalSections);

} // namespace neverd

#endif
