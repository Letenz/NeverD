//===- UnpackInternal.h - Private unpacking vocabulary ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_CORE_UNPACKINTERNAL_H
#define NEVERD_UNPACK_CORE_UNPACKINTERNAL_H

#include "neverd/unpack/Unpack.h"

#include "llvm/ADT/Twine.h"

namespace neverd::unpack {
namespace value {
#define NEVERD_UNPACK_VALUE(Name, Value) inline constexpr uint64_t Name = Value;
#include "UnpackValues.def"
#undef NEVERD_UNPACK_VALUE
} // namespace value
namespace text {
#define NEVERD_UNPACK_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_TEXT
#define NEVERD_UNPACK_PRIVATE_TEXT(Name, Text)                                 \
  inline constexpr char Name[] = Text;
#include "UnpackValues.def"
#undef NEVERD_UNPACK_PRIVATE_TEXT
} // namespace text
namespace field {
#define NEVERD_UNPACK_REPORT_FIELD(Name, Text)                                 \
  inline constexpr char Name[] = Text;
#define NEVERD_UNPACK_REPORT_LIMIT(Name, CName, Value)                         \
  inline constexpr uint64_t Name = Value;
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_REPORT_LIMIT
#undef NEVERD_UNPACK_REPORT_FIELD
} // namespace field

inline llvm::Error failure(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 text::Prefix + Message);
}
/// Read a complete input file under the unpacking size limit.
llvm::Expected<std::vector<uint8_t>>
readInput(const std::filesystem::path &Path);
/// The guest instruction set vocabulary of the execution layer, without a
/// link dependency on it.
const char *architectureName(emulation::GuestArchitecture Architecture);
} // namespace neverd::unpack
#endif
