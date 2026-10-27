//===- RegistrationEpilogue.h - Physical C++ exit projection -----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_REGISTRATIONEPILOGUE_H
#define NEVERD_IR_MED_X86_REGISTRATIONEPILOGUE_H

namespace neverd {
struct BinaryImage;
struct LowFunc;
namespace x86_registration {

/// Expand checked unguarded shared exits after projecting their entry frame.
/// Keep the original source receipts and the independent native EH gate.
void projectCxxEpilogues(const BinaryImage &Image, const LowFunc &Source,
                         LowFunc &Physical);

} // namespace x86_registration
} // namespace neverd

#endif
