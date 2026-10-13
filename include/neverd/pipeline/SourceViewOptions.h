//===- SourceViewOptions.h - Optional source transformations --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Request-local source transformations, independent of machine analysis and
/// the optimizer used for lifting, translation or patching.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PIPELINE_SOURCEVIEWOPTIONS_H
#define NEVERD_PIPELINE_SOURCEVIEWOPTIONS_H

#include <cstdint>

namespace neverd {

struct SourceViewOptions {
  enum class LLVMOptimization { Basic, O0, O1, O2, O3, Custom };
  enum Pass : uint32_t {
    InstCombine = 1,
    EarlyCSE = 2,
    SimplifyCFG = 4,
    DCE = 8,
  };
  static constexpr uint32_t BasicPasses =
      InstCombine | EarlyCSE | SimplifyCFG | DCE;

  bool Symbolic = false;
  LLVMOptimization LLVM = LLVMOptimization::Basic;
  uint32_t Passes = BasicPasses;
  bool operator==(const SourceViewOptions &) const = default;
};

} // namespace neverd

#endif // NEVERD_PIPELINE_SOURCEVIEWOPTIONS_H
