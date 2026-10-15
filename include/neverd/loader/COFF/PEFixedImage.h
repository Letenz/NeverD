//===- PEFixedImage.h - Evidence for fixed PE image bytes -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_PEFIXEDIMAGE_H
#define NEVERD_LOADER_COFF_PEFIXEDIMAGE_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace neverd {

struct BinaryImage;

struct PEFixedImageLimits {
  /// Aggregate validation work: raw/mapped bytes, names and metadata reads.
  /// Repeatedly inspected bytes are charged again, rather than only once per
  /// file occurrence. These are not ordinary PE loader limits.
  uint64_t MaxBytes = 64 * 1024 * 1024;
  /// Section, relocation, import and derived-provenance traversal work.
  uint64_t MaxRecords = 65536;
};

struct PEFixedImportIdentity {
  va_t Slot = 0;
  std::string Module;
  std::string Symbol;
};

/// A loader-validated snapshot at the PE's preferred image base. Preparing a
/// view authenticates the raw headers, complete scalar relocation inventory,
/// ordinary import write footprint, mapping identity and mapped file bytes.
/// Unknown loader writers, partial metadata and overlapping mappings refuse.
/// The image must outlive the view and remain unchanged while it is in use.
/// This certifies fixed snapshot execution, not Windows/DLL initialization or
/// equivalence at other load bases. Other immutable-image APIs retain their
/// existing contracts.
class PEFixedImageView {
  struct Storage;
  std::shared_ptr<const Storage> Data;
  explicit PEFixedImageView(std::shared_ptr<const Storage> Data);

public:
  static llvm::Expected<PEFixedImageView>
  create(const BinaryImage &Image, const PEFixedImageLimits &Limits = {});

  std::optional<llvm::ArrayRef<uint8_t>> read(va_t Address, uint32_t Bytes,
                                              bool Executable) const;
  /// Exact ordinary named import in a complete read-only slot. Identity comes
  /// from authenticated raw descriptors/thunks, never mutable display names.
  /// This neither reads nor certifies the runtime pointer stored in the IAT.
  std::optional<PEFixedImportIdentity> importIdentity(va_t Slot) const;
  /// Complete nonwrapping preferred-base extent from authenticated
  /// SizeOfImage, including headers and holes between section mappings.
  std::pair<va_t, va_t> imageExtent() const;
  const std::string &digest() const;
};

} // namespace neverd

#endif // NEVERD_LOADER_COFF_PEFIXEDIMAGE_H
