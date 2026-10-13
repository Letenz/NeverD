//===- Brotli.h - Bounded native Brotli stream decoding ----------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit in-process codec with allocation, output, call and time ceilings.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace neverd::web::compression {
bool brotliAvailable();
/// One complete RFC 7932 stream, no large-window extension or trailing bytes.
/// All counters are shared by the owning extraction. Failure discards output.
std::string decodeBrotli(std::string_view Input, uint64_t MaxOutput,
                         uint64_t Memory, uint64_t &Calls, uint64_t MaxCalls,
                         std::chrono::steady_clock::time_point Deadline);
} // namespace neverd::web::compression
