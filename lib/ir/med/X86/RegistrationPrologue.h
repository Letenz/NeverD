//===- RegistrationPrologue.h - Physical C++ entry projection ----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_REGISTRATIONPROLOGUE_H
#define NEVERD_IR_MED_X86_REGISTRATIONPROLOGUE_H

#include "neverd/ir/low/LowIR.h"

#include <optional>

namespace neverd {
struct BinaryImage;
namespace x86_registration {

/// Expand an image-bound shared entry call for stack analysis and SSA. The
/// source function and its EH receipts remain unchanged; this does not grant
/// native frame reconstruction or permission to omit cookies or chain writes.
std::optional<LowFunc> projectCxxPrologue(const BinaryImage &Image,
                                          const LowFunc &Source);

} // namespace x86_registration
} // namespace neverd

#endif
