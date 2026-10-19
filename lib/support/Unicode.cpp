//===- Unicode.cpp - Bulk UTF-8 validation
//---------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validates existing caller-owned buffers without replacing the allocator.
///
//===----------------------------------------------------------------------===//

#include "neverd/support/ByteScan.h"

#if NEVERD_USE_SIMDUTF
#include "simdutf.h"
#endif

bool neverd::knownValidUTF8(std::string_view Text) {
#if NEVERD_USE_SIMDUTF
  return Text.empty() || simdutf::validate_utf8(Text.data(), Text.size());
#else
  return false;
#endif
}
