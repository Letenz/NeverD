//===- LLVMInterpreterModel.h - Shared LLVM model contract -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_LLVMINTERPRETERMODEL_H
#define NEVERD_ANALYSIS_LLVMINTERPRETERMODEL_H

#include "neverd/analysis/InterpreterModel.h"

#include <cstdint>

namespace llvm {
class Function;
}

namespace neverd::analysis {

struct LLVMInterpreterModelLimits {
  uint64_t MaxInputItems = 65536;
  uint64_t MaxBlocks = 4096;
  uint64_t MaxOperations = 262144;
  /// Shared traversal, pointer-projection, initialization, allocation and
  /// emission work.
  uint64_t MaxWork = 1048576;
};

/// Reserved byte accumulating failed source-definedness obligations. It is
/// separate from adapter state storage, guest registers and LLVM values.
inline constexpr uint64_t LLVMInterpreterDefinednessOffset = uint64_t{1} << 39;

} // namespace neverd::analysis

#endif
