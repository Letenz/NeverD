//===- SEHFrameProofDetail.h - Checked LowIR frame CFG ------------------===//
// NeverD Decompiler
/// \file
/// Shared private validation for exact decoded frame propagation.
#ifndef NEVERD_IR_LOW_SEHFRAMEPROOFDETAIL_H
#define NEVERD_IR_LOW_SEHFRAMEPROOFDETAIL_H
#include "neverd/ir/low/LowIR.h"

#include <map>
namespace neverd {
bool completeLowSEHFrameCFG(const LowFunc &, const std::map<int, size_t> &,
                            size_t &WorkRemaining);
} // namespace neverd
#endif
