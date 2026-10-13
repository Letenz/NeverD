//===- PipelineSEHRuntimeMemory.h - Image-owned CRT effects -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Declares collection of checked Windows CRT source memory evidence.

#ifndef NEVERD_PIPELINE_SEHRUNTIMEMEMORY_H
#define NEVERD_PIPELINE_SEHRUNTIMEMEMORY_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/SEHRuntimeMemory.h"

namespace neverd {
struct BinaryImage;

/// Authenticate exact Windows x64 bindings and immutable scalar printf
/// formats used by these decoded bodies. An unsupported image or exhausted
/// scan returns an empty catalog. This grants no call argument or stack fact.
LowSEHRuntimeMemory
collectLowSEHRuntimeMemory(const BinaryImage &Image,
                           llvm::ArrayRef<LowFunc> Functions);
} // namespace neverd

#endif
