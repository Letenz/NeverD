//===- ReadOnlyBytesIndex.h - Private immutable PE32 query index -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_READONLYBYTESINDEX_H
#define NEVERD_LIB_LOADER_READONLYBYTESINDEX_H

#include "neverd/Common.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace neverd {
struct BinaryImage;
struct RelocatedAddressField;

namespace read_only_detail {
struct PE32CodeFixup {
  enum class Kind { Forbidden, Base, Operand } Type;
  va_t Address;
  uint8_t Width;
  uint8_t BaseType = 0;
  const RelocatedAddressField *Field = nullptr;
};
} // namespace read_only_detail

/// A scoped index over an immutable BinaryImage. The image and its metadata
/// must remain unchanged for this object's lifetime. This private accelerator
/// shares the ordinary reader's validator; it grants no additional authority.
class IndexedPE32CodeBytes {
public:
  IndexedPE32CodeBytes(const BinaryImage &Image, size_t &Work, size_t Limit);
  std::optional<std::vector<uint8_t>>
  read(va_t Address, uint32_t Size, const std::vector<va_t> &AbsoluteOperands);

private:
  bool charge(size_t Amount);
  const BinaryImage &Image;
  size_t &Work;
  size_t Limit;
  bool Valid = false;
  uint8_t MaximumWidth = 8;
  std::vector<read_only_detail::PE32CodeFixup> Fixups;
};
} // namespace neverd

#endif
