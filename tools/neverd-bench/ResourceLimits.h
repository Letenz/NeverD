//===- ResourceLimits.h - Signature benchmark defaults ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defaults for repeatable signature matching measurements.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_BENCH_RESOURCELIMITS_H
#define NEVERD_TOOLS_NEVERD_BENCH_RESOURCELIMITS_H

namespace neverd::bench {

inline constexpr unsigned kSignatureRepetitions = 7;
inline constexpr unsigned kSignatureThreads = 1;
inline constexpr char kSignatureRoot[] = "signatures";

} // namespace neverd::bench

#endif // NEVERD_TOOLS_NEVERD_BENCH_RESOURCELIMITS_H
