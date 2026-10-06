//===- UPXInternal.h - UPX facts shared by its container files --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_PACKERS_UPX_UPXINTERNAL_H
#define NEVERD_UNPACK_PACKERS_UPX_UPXINTERNAL_H

#include "../../core/Capture.h"
#include "../../format/pe/PEImage.h"

namespace neverd::unpack::upx {
namespace value {
#define NEVERD_UNPACK_UPX_VALUE(Name, Value)                                   \
  inline constexpr uint64_t Name = Value;
#define NEVERD_UNPACK_UPX_BYTES(Name, ...)                                     \
  inline constexpr uint8_t Name[] = {__VA_ARGS__};
#include "UPX.def"
#undef NEVERD_UNPACK_UPX_BYTES
#undef NEVERD_UNPACK_UPX_VALUE
} // namespace value
namespace text {
#define NEVERD_UNPACK_UPX_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "UPX.def"
#undef NEVERD_UNPACK_UPX_TEXT
} // namespace text

/// The format byte a pack header carries for \p Image's container and
/// instruction set. Absent when this module knows no stub for the pair.
std::optional<uint8_t> headerFormat(const InputImage &Image);
/// \p Bytes contain a complete pack header for \p Format whose checksum and
/// sizes are consistent. The magic alone is a four-byte string that any file
/// may contain.
bool hasPackHeader(llvm::ArrayRef<uint8_t> Bytes, uint8_t Format);

/// Append each independent UPX observation present in a PE image.
void collectEvidence(const pe::Image &Image,
                     std::vector<PackerEvidence> &Evidence);
/// The address an executable stub jumps to when it has finished. Every x64
/// executable stub ends by clearing the stack red zone and jumping to the
/// program it unpacked; the target is an operand of that jump, not an
/// estimate. Absent unless the sequence occurs exactly once after the entry
/// point and its target lies in the image.
std::optional<uint64_t> stubEntry(const pe::Image &Image);
/// The TLS directory of the unpacked program. A packed image carries its own
/// directory so that the system loader can allocate the slot before anything
/// is unpacked; its callback list holds only the stub's handler, which
/// forwards to the program's callbacks once the stub has run. An image that
/// starts at the program's entry never runs the stub, so the program's own
/// directory must be named again. It is the one record in the observed
/// memory, outside the packed directory, that names the same index cell and
/// template size. Absent when the input has no TLS directory or the record
/// is not unique.
std::optional<uint64_t> programTLSDirectory(const pe::Image &Image,
                                            const Capture &Observed);
} // namespace neverd::unpack::upx
#endif
