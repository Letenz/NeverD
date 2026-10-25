//===- SHA256Internal.h - Digest implementation selection -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private dispatch observation for native digest qualification. Tests may
/// compile SHA256.cpp with NEVERD_SHA256_PORTABLE to exercise the LLVM
/// fallback; product builds always retain runtime feature selection.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SUPPORT_SHA256INTERNAL_H
#define NEVERD_LIB_SUPPORT_SHA256INTERNAL_H

namespace neverd::detail {

/// Cached OS/CPU capability decision actually used by the one-shot owner.
bool sha256UsesHardware();

} // namespace neverd::detail

#endif // NEVERD_LIB_SUPPORT_SHA256INTERNAL_H
