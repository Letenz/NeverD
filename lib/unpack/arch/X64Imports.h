//===- X64Imports.h - x64 import witnesses -----------------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_ARCH_X64IMPORTS_H
#define NEVERD_UNPACK_ARCH_X64IMPORTS_H

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>

namespace neverd::unpack::x64 {
inline constexpr unsigned MinAddressLoadSize = 7;
inline constexpr unsigned MaxAddressLoadSize = 8;
/// A direct helper call, optionally preceded by a register PUSH or POP.
/// This identifies an instruction boundary to observe, never its semantics.
std::optional<unsigned> importHelper(llvm::ArrayRef<uint8_t> Memory,
                                     uint64_t InstructionRVA);
struct ImportSite {
  uint64_t RVA, Size;
  /// Encoding number of the destination GPR, for an address load.
  std::optional<unsigned> Register;
  /// The replacement instruction may leave a trailing NOP in the old site.
  uint64_t instructionEnd() const {
    return RVA + (Register ? MinAddressLoadSize : Size);
  }
};
/// Decode only the bounded form justified by the corresponding execution
/// witness. This never identifies a protector or predicts an export identity.
std::optional<ImportSite>
importSite(llvm::ArrayRef<uint8_t> Memory, uint64_t ReturnRVA, bool AddressLoad,
           uint64_t InstructionRVA,
           std::optional<unsigned> ResultRegister = std::nullopt);
void writeImport(llvm::MutableArrayRef<uint8_t> Memory, const ImportSite &Site,
                 uint64_t SlotRVA);
} // namespace neverd::unpack::x64
#endif
