//===- ResourceLimits.h - Benchmark defaults -------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defaults for repeatable component measurements.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_BENCH_RESOURCELIMITS_H
#define NEVERD_TOOLS_NEVERD_BENCH_RESOURCELIMITS_H

namespace neverd::bench {

inline constexpr unsigned kSignatureRepetitions = 7;
inline constexpr unsigned kSignatureIndexRepetitions = 1;
inline constexpr unsigned kSignatureThreads = 1;
inline constexpr char kSignatureRoot[] = "signatures";
inline constexpr unsigned kMedRepetitions = 7;
inline constexpr unsigned kMedThreads = 1;
inline constexpr unsigned kMedMaxFunctions = 0;

} // namespace neverd::bench

#endif // NEVERD_TOOLS_NEVERD_BENCH_RESOURCELIMITS_H
