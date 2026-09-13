//===- ExecutableCodeOwnerIndex.h - Scoped code ownership -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_EXECUTABLECODEOWNERINDEX_H
#define NEVERD_LOADER_EXECUTABLECODEOWNERINDEX_H

#include "neverd/Common.h"

#include <utility>
#include <vector>

namespace neverd {

struct BinaryImage;

/// Index the slow metadata lookups for one unchanged image operation. The
/// caller must rebuild after changing imports, symbols, ranges, mappings or
/// architecture/mode. Const queries may be shared by all operation workers;
/// no cached state is attached to the publicly mutable BinaryImage.
class ExecutableCodeOwnerIndex {
public:
  explicit ExecutableCodeOwnerIndex(const BinaryImage &Image);

private:
  friend struct BinaryImage;

  bool isImportStubAt(va_t Addr) const;
  bool hasKnownOrTypedOwnerAt(va_t Addr) const;
  va_t getFunctionMetadataEnd(va_t Entry) const;

  const BinaryImage *Image;
  std::vector<va_t> ImportStubs;
  std::vector<std::pair<va_t, va_t>> ImportRanges;
  std::vector<va_t> FunctionStarts;
  std::vector<std::pair<va_t, va_t>> CodeRanges;
  // Exact raw entries and their smallest finite end, without normalization
  // or merging overlapping functions into one owner.
  std::vector<std::pair<va_t, va_t>> FunctionMetadataEnds;
};

} // namespace neverd

#endif // NEVERD_LOADER_EXECUTABLECODEOWNERINDEX_H
