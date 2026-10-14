//===- SEHSourceRoots.h - Conservative source entry state -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retains unproved Windows machine-entry state in source analysis.

#ifndef NEVERD_IR_MED_X86_SEHSOURCEROOTS_H
#define NEVERD_IR_MED_X86_SEHSOURCEROOTS_H

#include "neverd/ir/med/MedIR.h"

namespace neverd {
struct LowFunc;
namespace x86_seh {
void seedSourceEntries(MedFunc &Func, const LowFunc &Low,
                       const std::vector<std::set<int>> &LiveIn,
                       const std::map<int, MedVar> &Variables, Arch TargetArch);
void mergeSourceHandlerStack(MedBlock &Block, const MedVar &Stack,
                             const std::vector<int> &Predecessors,
                             int VirtualRoot, Arch TargetArch);
} // namespace x86_seh
} // namespace neverd

#endif
