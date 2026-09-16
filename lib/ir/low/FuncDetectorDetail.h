//===- FuncDetectorDetail.h - Per-arch function scanners --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal declarations shared between FuncDetector.cpp and the
/// architecture-specific scanners (FuncDetectorX86.cpp, FuncDetectorARM.cpp,
/// FuncDetectorAArch64.cpp).  Not a public header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_FUNCDETECTORDETAIL_H
#define NEVERD_IR_LOW_FUNCDETECTORDETAIL_H

#include "neverd/decode/Decoder.h"
#include "neverd/loader/BinaryImage.h"

#include <set>

namespace neverd {
namespace func_detect_detail {

void scanSegmentCallsX86(const BinaryImage &Img, Decoder &Dec,
                         const Segment *Seg, va_t Start, va_t End,
                         std::set<va_t> &Out);
void scanSegmentCallsARM(const BinaryImage &Img, Decoder &Dec,
                         const Segment *Seg, va_t Start, va_t End,
                         std::set<va_t> &Out);
void scanSegmentCallsAArch64(const BinaryImage &Img, const Segment *Seg,
                             va_t Start, va_t End, std::set<va_t> &Out);

inline void scanSegmentCalls(const BinaryImage &Img, Decoder &Dec,
                             const Segment *Seg, va_t Start, va_t End,
                             std::set<va_t> &Out) {
  switch (Img.Arch) {
  case Arch::AArch64:
    scanSegmentCallsAArch64(Img, Seg, Start, End, Out);
    return;
  case Arch::ARM:
    scanSegmentCallsARM(Img, Dec, Seg, Start, End, Out);
    return;
  case Arch::X86:
  case Arch::X64:
    scanSegmentCallsX86(Img, Dec, Seg, Start, End, Out);
    return;
  default:
    scanSegmentCallsX86(Img, Dec, Seg, Start, End, Out);
    return;
  }
}

} // namespace func_detect_detail
} // namespace neverd

#endif // NEVERD_IR_LOW_FUNCDETECTORDETAIL_H
