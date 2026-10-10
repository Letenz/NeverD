//===- ResolverLaneView.h - Query-local register lane views ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_JUMPTABLE_RESOLVERLANEVIEW_H
#define NEVERD_IR_LOW_JUMPTABLE_RESOLVERLANEVIEW_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"

#include <array>
#include <cstdint>

namespace neverd::detail {

struct ResolverLaneView {
  VnodeSpace Space = VnodeSpace::CONST;
  uint64_t Container = InvalidVA;
  uint16_t ContainerSize = 0;
  uint16_t Begin = 0;
  uint16_t Size = 0;
  bool Valid = false;

  bool operator==(const ResolverLaneView &) const = default;
};

/// Register metadata is immutable during one proof query. Cache its lane
/// decomposition in fixed storage: collisions replace an entry after checking
/// the complete offset/width key, and cannot alias different register views.
/// No CFG/value proof or architecture-specific result survives the query.
class ResolverLaneViews {
  struct Entry {
    uint64_t Offset = 0;
    uint16_t Size = 0;
    ResolverLaneView View;
  };

  const TargetRegInfo &TRI;
  std::array<Entry, 64> Cache{};

public:
  explicit ResolverLaneViews(const TargetRegInfo &TRI) : TRI(TRI) {}

  ResolverLaneView get(const NdVar &V) {
    ResolverLaneView View;
    if (V.Size == 0 || (!V.isReg() && !V.isTemp()))
      return View;
    View.Space = V.Space;
    View.Size = V.Size;
    if (V.isTemp()) {
      View.Container = V.Offset;
      View.ContainerSize = V.Size;
      View.Valid = true;
      return View;
    }
    auto &Cached =
        Cache[(V.Offset ^ (V.Offset >> 3) ^ (uint64_t{V.Size} * 17)) %
              Cache.size()];
    if (Cached.Offset == V.Offset && Cached.Size == V.Size)
      return Cached.View;
    auto [WideOff, WideSize] = TRI.findWideReg(V.Offset, V.Size);
    int ByteOffset = TRI.subRegByteOffset(V.Offset, V.Size, WideOff, WideSize);
    if (ByteOffset < 0) {
      if (V.Offset < WideOff || V.Offset - WideOff > WideSize ||
          V.Size > WideSize - (V.Offset - WideOff))
        return View;
      ByteOffset = static_cast<int>(V.Offset - WideOff);
    }
    View.Container = WideOff;
    View.ContainerSize = WideSize;
    View.Begin = static_cast<uint16_t>(ByteOffset);
    View.Valid = true;
    Cached = {V.Offset, V.Size, View};
    return View;
  }
};

} // namespace neverd::detail

#endif
