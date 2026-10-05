//===- CSourceMap.h - Sidecar mapping of emitted C to library evidence ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_CSOURCEMAP_H
#define NEVERD_BACKEND_C_CSOURCEMAP_H

#include "neverd/ir/high/HighSourceMap.h"

namespace neverd {

class LLVMSourceMap;

struct CSourceSpan {
  /// Half-open UTF-8 byte offsets in the exact, complete emitted C text.
  size_t Begin = 0;
  size_t End = 0;
  bool operator==(const CSourceSpan &) const = default;
};

struct CSourceRegion {
  /// Index into CSourceMap::Recognitions, shared by both C routes.
  size_t Recognition = 0;
  std::vector<CSourceSpan> Spans;
  bool Mapped = false;
};

/// Per-emission inputs and output, separate from lifted IR and from C text.
/// Input snapshots must outlive emission. Regions is replaced on every emit;
/// absence of a surviving, complete mapping leaves a region unfolded.
struct CSourceMap {
  const std::vector<sigs::LibraryRecognition> *Recognitions = nullptr;
  const HighSourceMap *HighSources = nullptr;
  const LLVMSourceMap *LLVMSources = nullptr;
  std::vector<CSourceRegion> Regions;
};

} // namespace neverd

#endif
