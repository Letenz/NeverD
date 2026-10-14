//===- half_fma_host.c - Test binary16 FMA host provider ------------------===//
//--------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Provide test-only binary16 fused arithmetic with nearest-even rounding.
//===----------------------------------------------------------------------===//
// The host execution fixture uses default round-to-nearest, ties-to-even.
// Older host libraries lack fmaf16. This provider computes the finite result
// exactly in integers before one binary16 rounding. It supplies no product
// runtime, floating-environment effects or other rounding-mode contract.
//===----------------------------------------------------------------------===//

#include <stdint.h>

static __int128 half_scaled_integer(uint16_t Bits) {
  const unsigned Exponent = (Bits >> 10) & 31;
  const unsigned Fraction = Bits & 1023;
  const __int128 Magnitude =
      Exponent ? (__int128)(1024 + Fraction) << (Exponent - 1) : Fraction;
  return Bits & 0x8000 ? -Magnitude : Magnitude;
}

_Float16 fmaf16(_Float16 A, _Float16 B, _Float16 C) {
  const uint16_t ABits = __builtin_bit_cast(uint16_t, A);
  const uint16_t BBits = __builtin_bit_cast(uint16_t, B);
  const uint16_t CBits = __builtin_bit_cast(uint16_t, C);
  const uint16_t AAbs = ABits & 0x7fff;
  const uint16_t BAbs = BBits & 0x7fff;
  const uint16_t CAbs = CBits & 0x7fff;
  const uint16_t ProductSign = (ABits ^ BBits) & 0x8000;
  uint16_t Result;
  if (AAbs > 0x7c00)
    Result = ABits | 0x0200;
  else if (BAbs > 0x7c00)
    Result = BBits | 0x0200;
  else if (CAbs > 0x7c00)
    Result = CBits | 0x0200;
  else if ((AAbs == 0x7c00 && !BAbs) || (BAbs == 0x7c00 && !AAbs))
    Result = 0x7e00;
  else if (AAbs == 0x7c00 || BAbs == 0x7c00)
    Result = CAbs == 0x7c00 && (CBits & 0x8000) != ProductSign
                 ? 0x7e00
                 : ProductSign | 0x7c00;
  else if (CAbs == 0x7c00)
    Result = CBits;
  else {
    // Every finite binary16 value is an integer times 2^-24. The exact
    // product and sum therefore fit in 81 signed bits at scale 2^-48.
    const __int128 Exact =
        half_scaled_integer(ABits) * half_scaled_integer(BBits) +
        half_scaled_integer(CBits) * ((__int128)1 << 24);
    const uint16_t Sign = Exact < 0 ? 0x8000 : 0;
    const unsigned __int128 Magnitude = Exact < 0 ? -Exact : Exact;
    if (!Magnitude) {
      // Exact cancellation is +0 in the fixture's rounding mode. A sum of
      // like-signed zeros retains that sign.
      Result = (!AAbs || !BAbs) && !CAbs && (CBits & 0x8000) == ProductSign
                   ? ProductSign
                   : 0;
    } else {
      unsigned Top = 0;
      for (unsigned __int128 Remaining = Magnitude; Remaining >>= 1;)
        ++Top;
      unsigned Shift = Top > 34 ? Top - 10 : 24;
      const unsigned __int128 Discarded =
          Magnitude & (((unsigned __int128)1 << Shift) - 1);
      const unsigned __int128 Halfway = (unsigned __int128)1 << (Shift - 1);
      unsigned Rounded = (unsigned)(Magnitude >> Shift);
      Rounded += Discarded > Halfway || (Discarded == Halfway && (Rounded & 1));
      if (Rounded == 2048) {
        Rounded >>= 1;
        ++Shift;
      }
      const unsigned Exponent = Shift - 23;
      Result = Rounded < 1024   ? Sign | Rounded
               : Exponent >= 31 ? Sign | 0x7c00
                                : Sign | (Exponent << 10) | (Rounded - 1024);
    }
  }
  return __builtin_bit_cast(_Float16, Result);
}
