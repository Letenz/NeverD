//===- COFFRegistrationSEHStateWrites.h - Native stores --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks compiler state-write receipts against authenticated IR and bytes.

#ifndef NEVERD_COFFREGISTRATIONSEHSTATEWRITES_H
#define NEVERD_COFFREGISTRATIONSEHSTATEWRITES_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace llvm {
class Function;
}

namespace neverd {
struct CompiledImage;
struct ExceptionFunction;

/// Decode one unprefixed MOV dword ptr [base + Offset], State, returning the
/// architectural base register. No index, absolute address or trailing bytes
/// are permitted in the compiler-owned state-store instruction extent.
std::optional<uint8_t>
getCOFFRegistrationSEHStateWriteRegister(llvm::ArrayRef<uint8_t> Bytes,
                                         int32_t Offset, int32_t State);

/// Source IR and semantic rows must already be authenticated by their own
/// validators. This closes the remaining IR-to-MC-to-byte association.
llvm::Error
validateCOFFRegistrationSEHStateWrites(const llvm::Function &Parent,
                                       const ExceptionFunction &Source,
                                       const CompiledImage &Compiled);

} // namespace neverd

#endif
