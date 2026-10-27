//===- RegistrationSEHJumpPrivate.h - Jump closure ---------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_REGISTRATIONSEHJUMPPRIVATE_H
#define NEVERD_IR_MED_X86_REGISTRATIONSEHJUMPPRIVATE_H

#include <cstddef>

namespace neverd {
struct RegistrationStateAnalysis;

/// Every replacement names a checked callback occurrence and reaches a
/// completed capture or handler outcome through a bounded acyclic closure.
bool hasCompleteSEHJumpReplacements(const RegistrationStateAnalysis &States,
                                    size_t &Work);
} // namespace neverd

#endif
