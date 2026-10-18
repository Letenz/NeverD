//===- StackProbeIndex.h - Stack-probe helpers of an image -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIBC_STACKPROBEINDEX_H
#define NEVERD_LIBC_STACKPROBEINDEX_H

#include "neverd/libc/LibCNames.h"

#include <map>
#include <optional>

namespace neverd {

struct BinaryImage;

namespace libc {

/// stackProbeEffect() for one operation on an unchanged image.  The live
/// lookup scans every export and symbol for each call it is asked about.
/// Only an address that one of them names as a helper can be one, so those
/// few are decided once.  The owner must outlive its readers; a different
/// image uses the live lookup.
class StackProbeIndex {
public:
  explicit StackProbeIndex(const BinaryImage &Img);
  std::optional<StackProbeEffect> effect(const BinaryImage &Img,
                                         va_t Target) const;

private:
  const BinaryImage *Image;
  std::map<va_t, StackProbeEffect> Probes;
};

/// stackProbeEffect() answered through \p Index when one is available.
std::optional<StackProbeEffect> stackProbeEffect(const BinaryImage &Img,
                                                 va_t Target,
                                                 const StackProbeIndex *Index);

} // namespace libc
} // namespace neverd

#endif // NEVERD_LIBC_STACKPROBEINDEX_H
