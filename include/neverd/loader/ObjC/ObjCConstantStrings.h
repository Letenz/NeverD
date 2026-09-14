#ifndef NEVERD_LOADER_OBJC_OBJCCONSTANTSTRINGS_H
#define NEVERD_LOADER_OBJC_OBJCCONSTANTSTRINGS_H

#include "neverd/Common.h"

#include <optional>
#include <vector>

namespace neverd {
class BinaryImage;

/// One proven Darwin constant-string object. Units exclude the terminator;
/// UTF-16 code units are retained without a lossy Unicode conversion.
struct ObjCConstantString {
  bool UTF16 = false;
  std::vector<uint16_t> Units;
};

std::optional<ObjCConstantString>
readObjCConstantString(const BinaryImage &Image, va_t Address);
} // namespace neverd

#endif
