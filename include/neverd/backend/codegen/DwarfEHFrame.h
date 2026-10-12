//===- DwarfEHFrame.h - Regenerated DWARF unwind records -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_DWARFEHFRAME_H
#define NEVERD_BACKEND_CODEGEN_DWARFEHFRAME_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <vector>

namespace neverd {

struct DwarfEHFrameRecord {
  uint64_t BeginVA = 0;
  uint64_t EndVA = 0;
  uint64_t RecordVA = 0;
  /// Exact encoded initial-location field retained for native Mach-O linking.
  /// Darwin assemblers may resolve a section difference without a relocation.
  uint64_t InitialLocationOffset = 0;
  uint8_t InitialLocationEncoding = 0;
  uint8_t InitialLocationWidth = 0;
  bool HasLanguageAugmentation = false;

  bool covers(uint64_t Address) const {
    return BeginVA <= Address && Address < EndVA;
  }
};

/// Decode the CIE/FDE framing and PC ranges of a generated `.eh_frame` or
/// `__eh_frame` fragment at its final runtime address.
llvm::Expected<std::vector<DwarfEHFrameRecord>>
decodeDwarfEHFrameRecords(llvm::ArrayRef<uint8_t> Bytes, uint64_t BaseVA,
                          bool Is64BitAddress);

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_DWARFEHFRAME_H
