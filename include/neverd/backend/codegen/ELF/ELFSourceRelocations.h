//===- ELFSourceRelocations.h - Load-time replacement pointers -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_ELF_ELFSOURCERELOCATIONS_H
#define NEVERD_BACKEND_CODEGEN_ELF_ELFSOURCERELOCATIONS_H

#include "neverd/backend/codegen/BinaryRewriter.h"

namespace neverd {

/// An existing dynamic symbol, with its original version/dependency identity.
/// Never creates a new import or guesses a runtime address.
llvm::Expected<std::optional<uint32_t>>
findELFSourceDynamicSymbol(const BinaryImage &Original, llvm::StringRef Name);

struct ELFSourceRelocationTable {
  uint64_t VA = 0;
  uint64_t Size = 0;
  uint64_t AddressTagOffset = 0;
  uint64_t SizeTagOffset = 0;
  uint64_t SectionHeaderOffset = 0;
  bool Is64 = true;
  std::vector<uint8_t> ExpectedBytes;
};

struct ELFSourcePointerRelocation {
  uint64_t FieldVA = 0;
  /// Zero selects a base-relative pointer; otherwise an existing DT_SYMTAB
  /// index.
  uint32_t DynamicSymbol = 0;
  int64_t Addend = 0;
};

/// Both producer paths use this format-owned registration step. Fields must
/// occupy distinct, bounded eight-byte words in the new writable allocation.
llvm::Expected<ELFSourceRelocationTable> prepareELFPointerRelocations(
    const BinaryImage &Original, CompiledImage &Compiled,
    llvm::ArrayRef<ELFSourcePointerRelocation> Pointers,
    uint64_t MetadataOffset);

/// Register every emitted absolute pointer in allocated storage. The new
/// relocation table occupies reserved read-only padding; fields it updates
/// must be in the independently mapped RW tail. Mutates Compiled only after
/// all symbols, original dynamic metadata and placement have been validated.
llvm::Expected<ELFSourceRelocationTable>
prepareELFSourceRelocations(const BinaryImage &Original,
                            const llvm::Module &Module, CompiledImage &Compiled,
                            uint64_t MetadataOffset);

/// Publish the relocated table through the original PT_DYNAMIC and section
/// header after its PT_LOAD has been installed. Failure preserves Binary.
llvm::Error installELFSourceRelocations(std::vector<uint8_t> &Binary,
                                        const ELFSourceRelocationTable &Table);

/// Re-read authoritative dynamic tags and every old/new writer before
/// publication.
llvm::Error verifyELFSourceRelocations(const BinaryImage &Image,
                                       const ELFSourceRelocationTable &Table);

} // namespace neverd

#endif
