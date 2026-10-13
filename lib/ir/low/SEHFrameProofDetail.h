//===- SEHFrameProofDetail.h - Checked LowIR frame CFG ------------------===//
// NeverD Decompiler
/// \file
/// Shared private validation for exact decoded frame propagation.
#ifndef NEVERD_IR_LOW_SEHFRAMEPROOFDETAIL_H
#define NEVERD_IR_LOW_SEHFRAMEPROOFDETAIL_H
#include "neverd/ir/low/SEHFrameProof.h"

#include <map>
namespace neverd {
bool completeLowSEHFrameCFG(const LowFunc &, const std::map<int, size_t> &,
                            size_t &WorkRemaining);
// One immutable query reuses validated ordinary CFG provenance across its
// handlers. Protected prefixes and handler roles are still checked separately.
std::map<va_t, LowSEHEstablisherFrame>
proveLowSEHHandlerFrames(const LowFunc &, llvm::ArrayRef<va_t> Handlers,
                         const TargetRegInfo &, size_t &WorkRemaining,
                         llvm::StringRef LocalUnwindDigest);
} // namespace neverd
#endif
