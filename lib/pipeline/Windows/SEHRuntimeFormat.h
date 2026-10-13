//===- SEHRuntimeFormat.h - Windows CRT format contracts ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Declares immutable Windows scalar formats used by SEH frame proofs.

#ifndef NEVERD_PIPELINE_WINDOWS_SEHRUNTIMEFORMAT_H
#define NEVERD_PIPELINE_WINDOWS_SEHRUNTIMEFORMAT_H

#include "neverd/Common.h"

#include <cstddef>
#include <optional>
#include <string>

namespace neverd {
struct BinaryImage;

/// Return the checked format, including its terminator. This is a bounded
/// Windows runtime-memory contract, independent of source argument type hints.
std::optional<std::string> readWindowsSEHScalarFormat(const BinaryImage &Image,
                                                      va_t Address,
                                                      size_t &Remaining);

} // namespace neverd

#endif
