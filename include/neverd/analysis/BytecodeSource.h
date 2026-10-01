//===- BytecodeSource.h - Explicit bytecode state lowering ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_BYTECODESOURCE_H
#define NEVERD_ANALYSIS_BYTECODESOURCE_H

#include "neverd/analysis/BytecodeDecoder.h"
#include "neverd/ir/SourceTypeHint.h"

namespace neverd::analysis {

struct BytecodeStateFunction {
  LowFunc Function;
  SourceFunctionTypeHint SourceABI;
};

/// Bind byte-addressed virtual registers to a caller-owned state buffer.
/// Reads and writes use exactly the declared width, including unaligned and
/// overlapping ranges. Memory addresses in the bytecode remain runtime values.
/// Exact state reads are forwarded within a block; overlapping state writes
/// and potentially aliasing guest writes invalidate cached views. State stores
/// remain observable to guest accesses and calls; no disjointness is assumed.
/// The state buffer and every guest access must name accessible host storage;
/// this offline source route does not certify native execution or patching.
/// Every direct call must appear in BoundCalls. Its source contract is
/// uint64_t callee(void *state): it may update all state and guest memory;
/// zero continues, and nonzero propagates immediately to the caller. The
/// caller must provide identical source ABI hints when lowering these calls.
/// Unbound calls and register-indirect code pointers fail rather than being
/// reinterpreted as host pointers. RETURN denotes a language-level return as
/// specified by the profile, not an inferred native link-register contract.
/// Both existing C routes consume the returned LowIR and explicit source ABI.
/// Register storage is little endian; instruction-field byte order is separate.
llvm::Expected<BytecodeStateFunction>
lowerBytecodeState(const LowFunc &Function, uint32_t RegisterBytes,
                   Arch SourceArch = Arch::AArch64,
                   uint64_t MaxOperations = 10000000,
                   llvm::ArrayRef<va_t> BoundCalls = {});

} // namespace neverd::analysis

#endif
