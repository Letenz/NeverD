//===- SEHFrameProofDetail.h - Checked LowIR frame CFG ------------------===//
// NeverD Decompiler
/// \file
/// Shared private validation for exact decoded frame propagation.
#ifndef NEVERD_IR_LOW_SEHFRAMEPROOFDETAIL_H
#define NEVERD_IR_LOW_SEHFRAMEPROOFDETAIL_H
#include "neverd/ir/low/SEHFrameProof.h"

#include <map>
#include <optional>
namespace neverd {
bool completeLowSEHFrameCFG(const LowFunc &, const std::map<int, size_t> &,
                            size_t &WorkRemaining,
                            bool SourceProjection = false);
// The x87 context handle is the one function temporary a frame proof passes
// over: only the X87Context intrinsics touch it, as their context operand, so
// it never becomes an address, a stack or frame value, a branch target or a
// call argument. False for any other function temporary; nullopt once the
// budget runs out.
std::optional<bool>
lowFunctionTemporariesAreX87ContextHandles(const LowFunc &,
                                           size_t &WorkRemaining);
// Whether \p Block writes each function temporary before it reads one. The x87
// rewrite begins the context at the first operation of every root, so the
// dispatcher never enters a handler that reads an unbound handle.
std::optional<bool> lowBlockBindsFunctionTemporaries(const LowFunc &,
                                                     const LowBlock &Block,
                                                     size_t &WorkRemaining);
// One immutable query reuses validated ordinary CFG provenance across its
// handlers. Protected prefixes and handler roles are still checked separately.
std::map<va_t, LowSEHEstablisherFrame>
proveLowSEHHandlerFrames(const LowFunc &, llvm::ArrayRef<va_t> Handlers,
                         const TargetRegInfo &, size_t &WorkRemaining,
                         llvm::StringRef LocalUnwindDigest);
} // namespace neverd
#endif
