//===- InputDigest.h - SHA-256 of a loaded input file ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The SHA-256 digest every load takes of its input file.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_INPUTDIGEST_H
#define NEVERD_LOADER_INPUTDIGEST_H

#include "llvm/ADT/ArrayRef.h"

#include <array>
#include <cstdint>

namespace neverd {

/// The SHA-256 digest of \p Data, the same as llvm::SHA256::hash's.
///
/// Every load hashes its whole input, and the portable code reads about
/// 160 MB a second: longer than a PE's headers and tables take to parse.  On
/// an x86 processor with the SHA extensions the digest is computed with
/// them instead, several times faster.
std::array<uint8_t, 32> sha256(llvm::ArrayRef<uint8_t> Data);

} // namespace neverd

#endif // NEVERD_LOADER_INPUTDIGEST_H
