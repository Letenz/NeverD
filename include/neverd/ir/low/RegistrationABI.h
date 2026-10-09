//===- RegistrationABI.h - Checked PE32 registration call ABI -----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_REGISTRATIONABI_H
#define NEVERD_IR_LOW_REGISTRATIONABI_H

#include "neverd/loader/ExceptionCommon.h"

#include <optional>
#include <vector>

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// A leaf's two separate address domains: its private invocation frame and
/// the bounded object borrowed through entry ECX. Object offsets are relative
/// to that object, never invented image addresses or private stack offsets.
struct RegistrationObjectExtent {
  int32_t Begin = 0;
  int32_t End = 0;
  bool operator==(const RegistrationObjectExtent &) const = default;
};

struct RegistrationLeafCalleeABI {
  va_t Target = InvalidVA;
  uint32_t StackPopBytes = 0;
  std::vector<RegistrationObjectExtent> ECXReads;
  std::vector<RegistrationObjectExtent> ECXWrites;
  std::vector<ExceptionAddressRange> ImageReads;
  std::vector<ExceptionAddressRange> ImageWrites;
  std::vector<ExceptionAddressRange> CallerPCWrites;
};

/// Prove a complete returning PE32 leaf with a private stack, preserved
/// nonvolatile registers and nonescaping ECX object access. This describes
/// the callee only: each caller must still prove object bounds, initialization
/// and separation from its registration fields before using the projection.
std::optional<RegistrationLeafCalleeABI>
getCheckedX86RegistrationLeafCalleeABI(const BinaryImage &Image, va_t Target);

/// Native registration lowering currently emits caller-cleanup calls. Require
/// the source and every preserved direct callee to have that same stack
/// contract. Indirect targets, incomplete bodies and tail-only bodies provide
/// no such proof. The final writer replays this check from immutable input.
bool hasCallerCleanupRegistrationABI(
    const LowFunc &Function, const BinaryImage &Image,
    std::vector<ExceptionAddressRange> *CallerPCWrites = nullptr);
} // namespace neverd

#endif
