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

namespace neverd {

struct CallRegisterBody {
  std::vector<LowBlock> Blocks;
  bool Complete = false;
  bool UnsafeIndirectBranches = false;
};

struct CallRegisterBodyView {
  llvm::ArrayRef<LowBlock> Blocks;
  bool Complete;
  bool UnsafeIndirectBranches;

  explicit CallRegisterBodyView(const LowFunc &F)
      : Blocks(F.Blocks), Complete(F.hasCompleteLiftCoverage()),
        UnsafeIndirectBranches(!F.UnsafeIndirectBranchAddresses.empty()) {}
  explicit CallRegisterBodyView(const CallRegisterBody &F)
      : Blocks(F.Blocks), Complete(F.Complete),
        UnsafeIndirectBranches(F.UnsafeIndirectBranches) {}
};

} // namespace neverd

#endif // NEVERD_IR_LOW_CALLREGISTERBODY_H
