#ifndef NEVERD_LOADER_OBJC_OBJCMETHODLISTS_H
#define NEVERD_LOADER_OBJC_OBJCMETHODLISTS_H

#include "neverd/Common.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd {
struct BinaryImage;
namespace objc {
struct MethodRecord {
  va_t Address = 0;
  std::optional<va_t> Implementation;
  std::string Selector;
  std::string TypeEncoding;
};

/// Shared record layout for class implementations and protocol declarations.
/// An absent implementation is retained, never turned into a function seed.
std::optional<std::vector<MethodRecord>>
readMethodList(const BinaryImage &Image, va_t Address, size_t &Remaining,
               std::string &Diagnostic);
} // namespace objc
} // namespace neverd
#endif
