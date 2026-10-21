//===- CallRegisterBody.h - Inputs to call summaries ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The machine blocks and coverage facts read by register/argument summaries.
/// This projection is not a LowFunc and cannot be emitted or converted to SSA.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_CALLREGISTERBODY_H
#define NEVERD_IR_LOW_CALLREGISTERBODY_H

#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>

namespace neverd {

struct CallRegisterBody {
  std::vector<LowBlock> Blocks;
  bool Complete = false;
  bool UnsafeIndirectBranches = false;
  /// Another root can enter without the registers an ordinary predecessor
  /// leaves. Block-only summaries must not propagate its incoming constants.
  bool HasIndependentRoots = false;
};

struct CallRegisterBodyView {
  llvm::ArrayRef<LowBlock> Blocks;
  bool Complete;
  bool UnsafeIndirectBranches;
  bool HasIndependentRoots;

  explicit CallRegisterBodyView(const LowFunc &F)
      : Blocks(F.Blocks), Complete(F.hasCompleteLiftCoverage()),
        UnsafeIndirectBranches(!F.UnsafeIndirectBranchAddresses.empty()),
        HasIndependentRoots(
            std::any_of(F.ModuleAnalysisRoots.begin(),
                        F.ModuleAnalysisRoots.end(),
                        [&](va_t Root) { return Root != F.Entry; }) ||
            std::any_of(F.OrdinaryModuleAnalysisRoots.begin(),
                        F.OrdinaryModuleAnalysisRoots.end(),
                        [&](va_t Root) { return Root != F.Entry; })) {}
  explicit CallRegisterBodyView(const CallRegisterBody &F)
      : Blocks(F.Blocks), Complete(F.Complete),
        UnsafeIndirectBranches(F.UnsafeIndirectBranches),
        HasIndependentRoots(F.HasIndependentRoots) {}
};

} // namespace neverd

#endif // NEVERD_IR_LOW_CALLREGISTERBODY_H
