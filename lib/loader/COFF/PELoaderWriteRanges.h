//===- PELoaderWriteRanges.h - Loader write overlap -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared overlap policy for independently bounded loader writes and reads.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_PELOADERWRITERANGES_H
#define NEVERD_LOADER_COFF_PELOADERWRITERANGES_H

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace neverd::coff_loader {
struct LoaderRange {
  uint64_t Begin = 0;
  uint64_t End = 0;
};

struct LoaderProtectedRead {
  LoaderRange Bytes;
  std::optional<uint64_t> InPlaceWriter;
};

/// Writes must already be sorted and disjoint. An explicitly permitted writer
/// may replace exactly one complete read, never its terminator or a neighbor.
inline bool loaderWritesPreserveRead(llvm::ArrayRef<LoaderRange> Writes,
                                     const LoaderProtectedRead &Read) {
  auto It = std::lower_bound(
      Writes.begin(), Writes.end(), Read.Bytes.Begin,
      [](LoaderRange Write, uint64_t Begin) { return Write.End <= Begin; });
  for (; It != Writes.end() && It->Begin < Read.Bytes.End; ++It)
    if (!Read.InPlaceWriter || *Read.InPlaceWriter != It->Begin ||
        Read.Bytes.Begin != It->Begin || Read.Bytes.End != It->End)
      return false;
  return true;
}
} // namespace neverd::coff_loader

#endif
