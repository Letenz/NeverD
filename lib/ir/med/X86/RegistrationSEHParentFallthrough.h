//===- RegistrationSEHParentFallthrough.h - Parent exit binding -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind a proven parent fallthrough to its surviving source chain removal.

#ifndef NEVERD_IR_MED_X86_REGISTRATIONSEHPARENTFALLTHROUGH_H
#define NEVERD_IR_MED_X86_REGISTRATIONSEHPARENTFALLTHROUGH_H

#include <cstddef>

namespace neverd {
struct MedBlock;
struct MedOp;
struct RegistrationStateAnalysis;
struct RegistrationSEHInvocationExit;

bool isRegistrationSEHParentFallthrough(
    const MedBlock &Block, const MedOp &Op, const MedBlock &Target,
    const RegistrationStateAnalysis &States,
    const RegistrationSEHInvocationExit &Exit, size_t &Work);
} // namespace neverd

#endif
