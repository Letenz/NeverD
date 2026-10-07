//===- ImageStrings.h - The strings of a loaded image -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The strings of a loaded image as the C API publishes them: the JSON
/// options that choose encodings and a minimum length, and one scan of the
/// image's data under them, kept on the Session until the options or the
/// image change.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SDK_CAPI_IMAGESTRINGS_H
#define NEVERD_LIB_SDK_CAPI_IMAGESTRINGS_H

#include "neverd/Common.h"
#include "neverd/support/StringScan.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd::sdk {

struct Session;

/// A string of the image, without its terminator.
struct ImageString {
  va_t Address = 0;
  uint64_t Bytes = 0;
  unsigned Chars = 0;
  strings::Encoding Kind = strings::Encoding::ASCII;
  /// The characters as UTF-8.
  std::string Text;
};

/// One scan of the image and the options it was made with.
struct ImageStringScan {
  strings::ScanOptions Options;
  std::vector<ImageString> Strings;
};

/// Reads the JSON options of neverd_strings_ex_json (`encodings`,
/// `min_length`) into \p Options.  Returns why they are invalid, or an empty
/// string.  Null or empty text keeps the defaults.
std::string parseStringOptions(const char *Json, strings::ScanOptions &Options);

/// The strings of the loaded image under \p Options, in address order: data
/// segments whole, and the data sections of code segments.
const std::vector<ImageString> &
imageStrings(Session &S, const strings::ScanOptions &Options);

/// The string of \p Strings holding \p Address, if one does.
const ImageString *stringHolding(const std::vector<ImageString> &Strings,
                                 va_t Address);

} // namespace neverd::sdk

#endif // NEVERD_LIB_SDK_CAPI_IMAGESTRINGS_H
