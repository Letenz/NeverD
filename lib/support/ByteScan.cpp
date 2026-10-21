//===- ByteScan.cpp - Bounded vector text scanning
//-------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Runtime-dispatched ASCII run scanning with an exact scalar tail.
///
//===----------------------------------------------------------------------===//

#include "neverd/support/ByteScan.h"

#include "neverd/support/Limits.h"

#if NEVERD_USE_HIGHWAY
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "ByteScan.cpp"
#include "hwy/foreach_target.h"
#include "hwy/highway.h"

HWY_BEFORE_NAMESPACE();
namespace neverd::strings::detail {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

size_t asciiPrefix(const uint8_t *Data, size_t Size) {
  const hn::ScalableTag<uint8_t> D;
  const size_t Width = hn::Lanes(D);
  size_t I = 0;
  for (; Size - I >= Width; I += Width) {
    const auto V = hn::LoadU(D, Data + I);
    const auto Printable =
        hn::And(hn::Ge(V, hn::Set(D, 0x20)), hn::Lt(V, hn::Set(D, 0x7f)));
    const auto Space = hn::Or(
        hn::Eq(V, hn::Set(D, '\t')),
        hn::Or(hn::Eq(V, hn::Set(D, '\n')), hn::Eq(V, hn::Set(D, '\r'))));
    const intptr_t Stop =
        hn::FindFirstTrue(D, hn::Not(hn::Or(Printable, Space)));
    if (Stop >= 0)
      return I + static_cast<size_t>(Stop);
  }
  while (I < Size && isTextASCII(Data[I]))
    ++I;
  return I;
}
} // namespace HWY_NAMESPACE
} // namespace neverd::strings::detail
HWY_AFTER_NAMESPACE();
#endif

#if !NEVERD_USE_HIGHWAY || HWY_ONCE
namespace neverd::strings::detail {
#if NEVERD_USE_HIGHWAY
HWY_EXPORT(asciiPrefix);
#endif

size_t longTextASCIIPrefix(const uint8_t *Data, size_t Size) {
#if NEVERD_USE_HIGHWAY
  if (Size >= ::neverd::limits::support::kByteScanLongTextASCIIPrefixSizeLimit)
    return HWY_DYNAMIC_DISPATCH(asciiPrefix)(Data, Size);
#endif
  size_t I = 0;
  while (I < Size && isTextASCII(Data[I]))
    ++I;
  return I;
}
} // namespace neverd::strings::detail
#endif
