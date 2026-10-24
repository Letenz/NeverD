//===- FlowProgress.h - Finite execution ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Interruptible proof for acyclic bodies and versioned counter components.
/// v1/v2 use one linear pass; v3/v4 repeat SCC analysis for nested loops under
/// the same work and cancellation allowance. v4 groups shared counter paths.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_FLOWPROGRESS_H
#define NEVERD_DIFF_FLOWPROGRESS_H

#include "neverd/diff/DataFlow.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd::diff {
enum class FlowProgressMode : uint8_t {
  SelfLoop,
  MultiBlock,
  Nested,
  Branched
};
inline FlowProgressMode flowProgressMode(llvm::StringRef Profile) {
  if (Profile == LLVMBranchProgressDataFlowProfile)
    return FlowProgressMode::Branched;
  if (Profile == LLVMNestedProgressDataFlowProfile)
    return FlowProgressMode::Nested;
  return Profile == LLVMMultiBlockProgressDataFlowProfile
             ? FlowProgressMode::MultiBlock
             : FlowProgressMode::SelfLoop;
}
/// All graph references must already have passed structural validation.
FlowProgress
proveFlowProgress(const DataFlowGraph &Graph, llvm::StringRef Contract,
                  const DataFlowOptions &Options,
                  llvm::function_ref<void(uint64_t)> Charge,
                  FlowProgressMode Mode = FlowProgressMode::SelfLoop);
std::optional<std::string>
validateFlowProgress(const DataFlowGraph &Graph, const DataFlowOptions &Options,
                     llvm::function_ref<void(uint64_t)> Charge);
} // namespace neverd::diff

#endif // NEVERD_DIFF_FLOWPROGRESS_H
