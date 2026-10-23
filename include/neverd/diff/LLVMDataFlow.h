//===- LLVMDataFlow.h - Optimized machine SSA -------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded structural projection of verified LLVM integer SSA. This adapter
/// observes the ordinary machine pipeline; it does not optimize or prove it.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_LLVMDATAFLOW_H
#define NEVERD_DIFF_LLVMDATAFLOW_H

#include "neverd/diff/DataFlow.h"

namespace llvm {
class Function;
}
namespace neverd {
class LLVMSourceMap;
namespace diff {
class LLVMDataFlowUnsupported : public std::runtime_error {
public:
  explicit LLVMDataFlowUnsupported(const std::string &Reason)
      : std::runtime_error("optimized SSA unavailable: " + Reason) {}
};

/// Only direct emission observations surviving replacement/deletion are
/// retained. Unsupported contracts refuse the whole projection; interrupted
/// work never publishes a graph missing definitions or origins.
DataFlowGraph buildLLVMDataFlow(const llvm::Function &Function,
                                const LLVMSourceMap &Sources, uint64_t Entry,
                                llvm::StringRef Revision, llvm::StringRef ABI,
                                bool NativeComplete,
                                const DataFlowOptions &Options = {},
                                const ComparisonControl &Control = {});
} // namespace diff
} // namespace neverd

#endif // NEVERD_DIFF_LLVMDATAFLOW_H
