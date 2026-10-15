//===- RegistrationSourceCall.h - PE32 source call binding ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONSOURCECALL_H
#define NEVERD_IR_LOW_X86_REGISTRATIONSOURCECALL_H

#include <cstddef>

namespace neverd {
struct BinaryImage;
struct LowFunc;
struct LowInstructionBoundary;
struct LowOp;

namespace registration_state {

/// Authenticate one owned direct E8 occurrence and its immutable bytes. This
/// grants no callee effects, stack cleanup, return or exception authority.
const LowInstructionBoundary *
checkedRegistrationDirectCall(const BinaryImage &Image, const LowFunc &Function,
                              const LowOp &Op, size_t &Work);

} // namespace registration_state
} // namespace neverd

#endif
