//===- RegistrationABI.h - Checked PE32 registration call ABI -----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_REGISTRATIONABI_H
#define NEVERD_IR_LOW_REGISTRATIONABI_H

#include "neverd/loader/ExceptionCommon.h"

#include <vector>

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Native registration lowering currently emits caller-cleanup calls. Require
/// the source and every preserved direct callee to have that same stack
/// contract. Indirect targets, incomplete bodies and tail-only bodies provide
/// no such proof. The final writer replays this check from immutable input.
bool hasCallerCleanupRegistrationABI(
    const LowFunc &Function, const BinaryImage &Image,
    std::vector<ExceptionAddressRange> *CallerPCWrites = nullptr);
} // namespace neverd

#endif
