//===- COFFRegistrationScalarLeaf.h - Retained PE32 scalar helpers -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LOADER_COFF_REGISTRATIONSCALARLEAF_H
#define NEVERD_LOADER_COFF_REGISTRATIONSCALARLEAF_H

#include "neverd/Common.h"

namespace neverd {
struct BinaryImage;

namespace coff_loader {
/// Prove a bounded cdecl scalar leaf cannot observe its caller's frame.
/// The original code and dynamic image reads remain in place. This grants
/// neither permission to copy the helper nor a constant result for its reads.
bool isCheckedX86RegistrationScalarLeaf(const BinaryImage &Image, va_t Entry);
} // namespace coff_loader
} // namespace neverd
#endif
