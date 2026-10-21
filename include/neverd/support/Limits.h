//===- Limits.h - Component limits and defaults -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resource ceilings, work budgets and defaults owned by this component.
/// Binary layouts and instruction/ABI constants remain with their models.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_LIMITS_H
#define NEVERD_SUPPORT_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd {

// include/neverd/support/CaseList.h
/// The shortest consecutive run written as a range.
inline constexpr std::size_t CaseRangeMinimum = 3;

} // namespace neverd

namespace neverd::strings {

// include/neverd/support/StringScan.h
/// The largest minimum length a scan accepts.
inline constexpr unsigned MaxMinLength = 1024;

} // namespace neverd::strings

namespace neverd::limits::support {

// lib/support/TextEvidence.cpp
inline constexpr size_t kTextEvidenceMinTableAscentIdeographs = 3;
inline constexpr unsigned kTextEvidenceMinWordLetters = 3;
inline constexpr unsigned kTextEvidenceMinLettersBeyond = 2;
inline constexpr unsigned kTextEvidenceMinDistinctASCIILetters = 3;

// lib/support/TextEvidence.h
/// Text mostly beyond ASCII needs this many characters: two random code
/// points read as text too often.
inline constexpr size_t kTextEvidenceMinTextBeyondASCII = 3;

// lib/support/ByteScan.cpp
inline constexpr int kByteScanLongTextASCIIPrefixSizeLimit = 32;

// lib/support/TextEncoding.cpp
inline constexpr int kTextEncodingEscapeInvalidUTF8TextSizeLimit = 64;

// Default display-column length for binary string discovery, shared by every
// surface.
inline constexpr unsigned kDefaultStringMinLength = 4;
} // namespace neverd::limits::support

#endif // NEVERD_SUPPORT_LIMITS_H
